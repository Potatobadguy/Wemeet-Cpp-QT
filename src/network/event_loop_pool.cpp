#include "event_loop_pool.h"
#include "logger.h"

namespace wemeet {

/**
 * @brief 构造函数：创建主 Reactor 与 N 个子 Reactor 对象（不启动线程）
 *
 * 参数解析：
 *  - num_loops 为 0 时自动推导子 loop 数量：
 *      std::thread::hardware_concurrency() 返回 CPU 逻辑核数；
 *      若返回 0（该值本身可能不可用），则兜底使用 4。
 *
 * 实现细节：
 *  1. main_loop_ 作为主 Reactor 先创建，用于后续 accept 新连接。
 *  2. loops_ 中创建 num_loops 个 EventLoop，并 reserve 提前预留容量，
 *     避免 vector 扩容时多次拷贝 unique_ptr（拷贝开销小，但避免
 *     realloc 期间的地址变动带来不必要的缓存失效）。
 *  3. 此处【只创建对象、不创建线程】——线程的创建统一推迟到 start()，
 *     保证"先构造完所有 loop 资源，再并行启动"的确定性。
 *
 * 注意：
 *  - EventLoop 的构造函数内部会创建 epoll_fd / wakeup_fd 等系统资源，
 *    因此构造函数可能抛出系统调用异常，调用方需注意异常安全。
 */
EventLoopPool::EventLoopPool(size_t num_loops) {
    // 未指定数量时，按 CPU 逻辑核数自适应，超线程/多核机器上
    // 每个核跑一个子 Reactor，最大化并行 I/O 能力
    if (num_loops == 0) {
        num_loops = std::thread::hardware_concurrency();
        // hardware_concurrency() 可能返回 0（实现不可用），兜底到 4
        if (num_loops == 0) num_loops = 4;
    }

    // 主 Reactor：用于 accept 新连接，事件循环跑在调用线程上
    main_loop_ = std::make_unique<EventLoop>();

    // 预分配容器容量，减少 vector 扩容次数
    loops_.reserve(num_loops);
    // 批量创建子 Reactor（此时线程尚未启动）
    for (size_t i = 0; i < num_loops; ++i) {
        loops_.push_back(std::make_unique<EventLoop>());
    }

    LOG_INFO("EventLoopPool created: main + %zu worker loops", num_loops);
}

/**
 * @brief 析构函数：停止所有线程并释放资源
 *
 * 委托给 stop() 完成：
 *  - stop() 内部会 quit 所有 loop 并 join 所有线程，
 *    确保对象析构时不会残留仍在运行的线程访问已销毁的成员。
 */
EventLoopPool::~EventLoopPool() {
    stop();
}

/**
 * @brief 启动所有子 Reactor 线程
 *
 * 为每个 worker loop 创建一条线程，线程入口直接调用 loop()：
 *  - loop() 内部进入 epoll_wait 死循环（阻塞运行），
 *    直到收到 quit() 才退出；
 *  - lambda 捕获 this 与下标 i，通过索引访问 loops_[i]，
 *    避免在闭包里持有裸指针的拷贝而引发歧义。
 *
 * 线程生命周期说明：
 *  - start() 返回后线程仍处于运行状态（epoll_wait 阻塞中）；
 *  - 线程的回收统一由 stop() 中的 join() 完成；
 *  - 创建多个线程的成本（内核 TCB、栈空间）比单线程稍高，
 *    但在高并发连接场景下收益远大于开销。
 */
void EventLoopPool::start() {
    for (size_t i = 0; i < loops_.size(); ++i) {
        // 每线程绑定一个 loop：threads_ 与 loops_ 按下标一一对应
        threads_.emplace_back([this, i]() {
            // 进入该子 Reactor 的事件循环（阻塞，直到 quit）
            loops_[i]->loop();
        });
    }
    LOG_INFO("EventLoopPool started: %zu worker threads", threads_.size());
}

/**
 * @brief 停止所有线程（优雅退出）
 *
 * 流程拆解：
 *  1. 通知退出：对每个 worker loop 调用 quit()。
 *     quit() 内部会通过 wakeup_fd（eventfd）写入数据唤醒阻塞中的
 *     epoll_wait，随后 loop 处理完当前批次就绪事件后退出循环；
 *     该过程是幂等的，重复调用无副作用。
 *  2. 主 loop 同样退出（若其事件循环在运行）。
 *  3. 回收线程：对每个线程执行 join()（仅当可 join，即线程已启动），
 *     保证 start() 创建的所有线程都执行完毕后再清理容器，避免
 *     悬空线程在对象析构后继续访问成员变量。
 *
 * 线程安全：
 *  - 本函数应在持有线程生命周期所有权的一方调用（通常是主线程），
 *    与 start() 成对出现，不支持并发调用。
 *  - 析构函数也会调用本函数，因此必须保证 stop() 可被重复调用
 *    （joinable 判断 + threads_.clear() 已保证）。
 */
void EventLoopPool::stop() {
    // 第一步：通知所有子 Reactor 退出事件循环
    for (auto& loop : loops_) {
        loop->quit();
    }
    // 第二步：通知主 Reactor 退出（若其 loop() 正在运行）
    if (main_loop_) {
        main_loop_->quit();
    }
    // 第三步：等待所有子线程真正结束，防止悬空线程
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    // 清理线程句柄容器，允许后续再次 start() 重新创建线程
    threads_.clear();
    LOG_INFO("EventLoopPool stopped");
}

/**
 * @brief 获取下一个 EventLoop（Round-Robin 轮询分发）
 *
 * 分发算法：
 *  - next_index_ 是原子计数器，fetch_add(1) 原子地返回旧值并自增；
 *  - 旧值 % loops_.size() 得到本次分发的 loop 下标；
 *  - 多次调用依次得到 0,1,2,...,n-1,0,1,...，连接被均匀打散到
 *    所有子 Reactor 上，避免单个 loop 上的连接数堆积过多。
 *
 * 使用场景：
 *  - 主 Reactor 在 accept 到新连接后调用本函数选择一个子 Reactor，
 *    再把新连接的 fd 通过 run_in_loop() 注册到该子 Reactor 上；
 *  - 由于基于原子计数，多个 accept 线程并发调用也安全。
 *
 * 内存序说明：
 *  - std::memory_order_relaxed（宽松序）已足够：我们只要求计数器
 *    自身不重不漏（原子性），不关心它与其他内存读写之间的先后顺序，
 *    因此选择开销最小的 relaxed 序即可。
 *
 * 边界条件：
 *  - 必须在 start() 之后、stop() 之前调用（loops_ 非空）；
 *    若 loops_ 为空（从未配置 loop），取模会除零，调用方需保证
 *    构造时传入了合法的 num_loops。
 *
 * @return 选中的 worker EventLoop 裸指针，生命周期由本类（loops_）持有
 */
EventLoop* EventLoopPool::next_loop() {
    // 轮询分发（Round-Robin）：原子自增取模，保证并发安全
    size_t idx = next_index_.fetch_add(1, std::memory_order_relaxed) % loops_.size();
    return loops_[idx].get();
}

} // namespace wemeet
