#pragma once
#include "event_loop.h"
#include <vector>
#include <thread>
#include <memory>
#include <atomic>

namespace wemeet {

/**
 * @brief One Loop Per Thread 事件循环池 — Reactor 多线程模型
 *
 * ─────────────────────────────────────────────────────────────
 *  架构总览
 * ─────────────────────────────────────────────────────────────
 *  本类是服务端网络模块的核心调度器，采用经典的多 Reactor 多线程模型：
 *
 *        ┌─────────────────────────────────────────────┐
 *        │              EventLoopPool                  │
 *        │                                             │
 *        │   main_loop_  (主 Reactor，跑在调用线程)      │
 *        │     └─ 负责 accept 新连接，然后分发          │
 *        │                                             │
 *        │   loops_[0]  ── 子 Reactor ── 线程 0         │
 *        │   loops_[1]  ── 子 Reactor ── 线程 1         │
 *        │   ...                                        │
 *        │   loops_[n]  ── 子 Reactor ── 线程 n         │
 *        │     └─ 负责已建立连接的 I/O 读写             │
 *        └─────────────────────────────────────────────┘
 *
 *  技术亮点：
 *   - 主 Reactor 负责 accept 新连接（只做连接建立，不阻塞于业务 I/O）
 *   - 子 Reactor 负责 I/O 读写，通过轮询（Round-Robin）算法分发连接，
 *     使得每个连接的读写任务被平均分配到各个子 Reactor 线程上
 *   - 每个子 Reactor 运行在独立线程中，多个线程并行处理 I/O，
 *     充分利用多核 CPU，突破单线程事件循环的吞吐量瓶颈
 *   - 主线程（调用 start() 的线程）一般不参与事件循环，
 *     main_loop_ 主要服务于需要绑定在单一线程上的操作（如 accept、信号处理）
 *
 *  ─────────────────────────────────────────────────────────────
 *  线程安全说明
 * ─────────────────────────────────────────────────────────────
 *   - EventLoopPool 本身不要求线程安全：start/stop/next_loop 通常在
 *     主线程（accept 线程）串行调用。
 *   - 唯一跨线程访问的是原子计数器 next_index_（std::atomic<size_t>），
 *     它保证多个线程同时调用 next_loop() 时也能正确完成轮询分发。
 *   - 每个子 EventLoop 的内部状态由各自线程独占，通过 EventLoop 的
 *     run_in_loop() 跨线程投递任务，天然规避了数据竞争。
 */
class EventLoopPool {
public:
    /**
     * @brief 构造函数
     *
     * @param num_loops 子 Reactor（worker loop）的数量。
     *        传入 0 表示使用 CPU 逻辑核数作为默认值：
     *          - 优先取 std::thread::hardware_concurrency()（逻辑核数）
     *          - 若该值不可用（返回 0，如某些受限环境），则退化为 4
     *
     * 注意：
     *  - 构造函数只创建 EventLoop 对象，【不会】启动线程，
     *    真正的线程创建发生在 start() 中。
     *  - 子 loop 数量建议不超过逻辑核数，避免线程过多造成上下文切换开销。
     */
    explicit EventLoopPool(size_t num_loops = 0);

    /**
     * @brief 析构函数
     *
     * 自动调用 stop() 停止所有线程并回收资源。
     * 由于 stop() 内部会 join 所有线程，析构是安全的
     * （不会出现线程在对象析构后仍在访问成员的情况）。
     */
    ~EventLoopPool();

    // 不可拷贝（对象持有线程句柄与唯一资源，拷贝无意义且危险）
    EventLoopPool(const EventLoopPool&) = delete;
    EventLoopPool& operator=(const EventLoopPool&) = delete;

    /**
     * @brief 启动所有子 Reactor 线程
     *
     * 为每一个 worker loop 创建一个 std::thread，线程入口为
     * loops_[i]->loop()，即阻塞在该 EventLoop 的 epoll_wait 上。
     *
     * 线程创建完成后立即返回（不等待线程内部初始化完成），
     * 因此线程创建数量由 loops_.size() 决定。
     *
     * 调用约定：
     *  - 通常在进程启动后、开始 accept 新连接之前调用一次。
     *  - 重复调用会造成线程重复创建，调用方需自行保证只调用一次。
     */
    void start();

    /**
     * @brief 停止所有线程
     *
     * 工作流程（三步）：
     *  1. 对每个 worker loop 调用 quit()，通过 eventfd 写入唤醒
     *     epoll_wait，使各线程的事件循环优雅退出（幂等，重复调用安全）。
     *  2. 对 main_loop_ 同样调用 quit()。
     *  3. join 所有子线程，确保线程执行完毕后再清理线程容器。
     *
     * 注意：
     *  - quit() 是"优雅退出"——会先处理完当前批次已就绪的事件，
     *    再退出循环，避免丢失在途数据。
     *  - stop() 与 start() 不可重入并发调用；对象析构时也会调用 stop()，
     *    因此 stop() 内部做了 joinable 判断以兼容重复调用。
     */
    void stop();

    /**
     * @brief 获取下一个 EventLoop（轮询分发）
     *
     * 采用 Round-Robin（轮询）算法：
     *  - next_index_ 是一个原子计数器，fetch_add(1) 原子自增并返回旧值；
     *  - 用旧值对 loops_.size() 取模，得到本次要分发的 loop 下标；
     *  - 这样连接会依次被分到 loops_[0]、loops_[1]、... 循环往复，
     *    实现负载在所有子 Reactor 上的平均分配。
     *
     * 线程安全：
     *  - std::memory_order_relaxed 即可满足需求：我们只要求计数的
     *    原子递增（不重复、不丢失），不要求与其他内存操作的先后顺序。
     *  - 因此本函数可以被多个线程（如多线程 accept）并发调用。
     *
     * @return 指向下一个 worker EventLoop 的裸指针（生命周期由本类持有）
     */
    EventLoop* next_loop();

    /**
     * @brief 获取主 EventLoop（用于 accept）
     *
     * 主 Reactor 运行在调用线程（通常为主线程），
     * 用于监听 listen fd 的读事件以 accept 新连接。
     *
     * 注意：主 loop 没有专属线程，需要调用方自行在合适的线程上
     * 调用 main_loop()->loop() 启动其事件循环。
     *
     * @return 主 EventLoop 的裸指针
     */
    EventLoop* main_loop() { return main_loop_.get(); }

    /**
     * @brief 获取子 Reactor（worker loop）的总数
     * @return worker loop 的数量（不含 main_loop_）
     */
    size_t size() const { return loops_.size(); }

private:
    std::unique_ptr<EventLoop>               main_loop_;    // 主 Reactor：负责 accept 新连接
    std::vector<std::unique_ptr<EventLoop>>  loops_;        // 子 Reactor 集合：负责连接 I/O，每个各跑一个线程
    std::vector<std::thread>                 threads_;      // 子 Reactor 对应的线程集合（与 loops_ 一一对应）
    std::atomic<size_t>                      next_index_{0}; // 轮询计数器：原子自增，支持多线程并发取 loop
};

} // namespace wemeet
