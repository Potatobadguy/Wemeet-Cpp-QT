#pragma once
#include <string>
#include <shared_mutex>
#include <unordered_map>
#include <array>
#include <memory>
#include <atomic>
#include <cstdint>

namespace wemeet {

/**
 * @brief 在线用户管理器 — 16 分片锁，读多写少优化
 *
 * ─────────────────────────────────────────────────────────────
 *  为什么需要分片锁？
 * ─────────────────────────────────────────────────────────────
 *  在线用户表会被【所有子 Reactor 线程】并发访问（信令服务器里
 *  每个连接的业务处理都会查/改在线状态），如果整个表只挂一把
 *  全局锁：
 *
 *     全局锁模型                        分片锁模型
 *   ┌───────────────┐              ┌─┐ ┌─┐ ┌─┐      ┌─┐
 *   │  一个 map      │              │0│ │1│ │2│ ...  │15│  ← 16 个小 map
 *   │  一把锁(串行)  │              │ │ │ │ │ │      │ │
 *   │  所有线程排队  │              └─┘ └─┘ └─┘      └─┘
 *   └───────────────┘               ↑↑↑  每片独立锁，互不阻塞
 *      并发度 = 1                       并发度 ≈ 16
 *
 *  分片（sharding）的核心思想：**把一把大锁拆成多把小锁，让不同的
 *  线程锁不同的锁**。每个 user_id 经 shard_index() 固定映射到其中
 *  一片，操作不同片的线程彼此完全并行，锁竞争被摊薄 16 倍。
 *
 *  技术亮点：
 *   - shared_mutex 读写锁：读操作（查找）共享锁，写操作独占锁
 *     —— 本项目"读多写少"（在线人数几千、查找频繁、增删偶发），
 *        读读之间不互斥，多个线程可同时查不同的用户
 *   - 16 分片减少锁竞争：代价只是 16 把锁 + 16 个桶，开销极小
 *   - 原子计数器跟踪在线人数：避免"数 16 片"需要锁全部片的尴尬
 *
 * ─────────────────────────────────────────────────────────────
 *  注意：本项目只提供"单 key 操作"（增/删/查单用户），不提供
 *  "遍历所有在线用户"的接口——因为遍历需要依次锁全部 16 片，
 *  那才是分片锁的弱项（会退化为全局锁的串行度）。
 * ─────────────────────────────────────────────────────────────
 */

/**
 * @brief 会话信息 — 一名在线用户的完整上下文
 */
struct SessionInfo {
    uint64_t    user_id;      // 用户 ID（分片键：决定落入哪个 shard）
    std::string nickname;     // 昵称（响应查询时回显）
    std::string token;        // 登录令牌（鉴权/防伪造）
    uint64_t    conn_id;      // 关联的 TCP 连接 ID（用于定位连接、踢人等）
    int64_t     login_time;   // 登录时刻（毫秒时间戳，可用于超时判定）
};

class UserManager {
public:
    UserManager() = default;

    // ── 会话管理 ─────────────────────────────────────────
    /**
     * @brief 新增/更新在线会话（写操作 → 独享锁）
     *
     * 同一 user_id 重复调用等价于"覆盖更新"（map 赋值语义），
     * 适合登录/重登场景。调用方需保证 session.user_id 合法。
     */
    void add_session(const SessionInfo& session);

    /**
     * @brief 移除在线会话（写操作 → 独享锁）
     *
     * 幂等：用户不在线时调用无副作用（erase 返回 0，不扣计数）。
     */
    void remove_session(uint64_t user_id);

    /**
     * @brief 查询会话（读操作 → 共享锁，多线程可并发调用）
     *
     * 返回 shared_ptr 而非裸指针：调用方持有时即使会话被并发移除，
     * 对象也不会被提前释放（shared_ptr 所有权转移）。
     *
     * @return 命中则返回会话对象，未在线返回 nullptr
     */
    std::shared_ptr<SessionInfo> get_session(uint64_t user_id);

    /**
     * @brief 更新在线状态
     *
     * online=true 时仅记录调试日志（在线状态以 add_session 为准）；
     * online=false 时等价于 remove_session。
     */
    void set_online(uint64_t user_id, bool online);

    // ── 统计 ─────────────────────────────────────────────
    /**
     * @brief 获取在线人数
     *
     * 由原子计数器维护，无需加锁、无需遍历分片，O(1) 获取。
     * 注意：这是一个"近似值"——并发增删时的读值可能落后几个计数，
     * 但对于统计展示场景足够精确。
     */
    size_t online_count() const {
        return online_count_.load(std::memory_order_acquire);
    }

private:
    // 分片数量：16 片。
    // 选择依据：分片太少→竞争摊薄不够；太多→锁与桶的开销变大、
    // 且单片的 map 过小失去意义。16 是在"摊薄竞争"与"开销可控"
    // 之间的常用折中（配合 shared_mutex 读共享，实际并发度更高）。
    static constexpr size_t kShardCount = 16;

    /**
     * @brief 单个分片 = 一把锁 + 一个小 map
     *
     * mutable：读方法 get_session 只持 shared_lock，也允许在
     * const 上下文中上锁。
     */
    struct Shard {
        mutable std::shared_mutex mutex;                                    // 读写锁
        std::unordered_map<uint64_t, std::shared_ptr<SessionInfo>> sessions; // 本片负责的用户
    };

    /**
     * @brief 分片路由：user_id → 片下标
     *
     * 用 user_id % kShardCount 取模。要求：
     *  - 确定性：同一 user_id 永远落到同一片（否则查不到）
     *  - 均匀性：user_id 足够分散时，各片负载近似均匀
     *    （连续自增的 user_id 对 16 取模正好均匀分布）
     *
     * 注意：kShardCount 必须为 2 的幂时取模才等价于位运算，
     * 16 = 2^4，编译器会优化成 & 15，开销可忽略。
     */
    size_t shard_index(uint64_t user_id) const {
        return user_id % kShardCount;
    }

    // 16 个分片（固定数组，栈上/对象内存中，无动态分配）
    std::array<Shard, kShardCount> shards_;
    // 在线人数原子计数器：增删时 fetch_add/fetch_sub 维护
    std::atomic<size_t>            online_count_{0};
};

} // namespace wemeet
