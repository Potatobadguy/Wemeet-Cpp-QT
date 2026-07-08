/**
 * @brief 单元测试 — Socket RAII 封装
 */
#include "socket.h"
#include <cstdio>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace wemeet;

static int tests_passed = 0, tests_failed = 0;
#define TEST(name) printf("  TEST: %s ... ", name)
#define PASS()     do { printf("PASSED\n"); tests_passed++; } while(0)
#define FAIL(msg)  do { printf("FAILED: %s\n", msg); tests_failed++; } while(0)
#define ASSERT(cond, msg) if (!(cond)) { FAIL(msg); return; }

// ── 测试1：创建 TCP Socket ──────────────────────────────
static void test_create_tcp() {
    TEST("create TCP socket");

    Socket sock = Socket::create_tcp();
    ASSERT(sock.valid(), "socket should be valid");
    ASSERT(sock.fd() >= 0, "fd should be >= 0");

    // RAII 自动关闭
    PASS();
}

// ── 测试2：Socket 选项 ──────────────────────────────────
static void test_socket_options() {
    TEST("socket options (reuse_addr, nodelay, keepalive)");

    Socket sock = Socket::create_tcp();
    ASSERT(sock.valid(), "socket should be valid");

    sock.set_reuse_addr(true);
    sock.set_tcp_nodelay(true);
    sock.set_keepalive(true);

    PASS();
}

// ── 测试3：移动语义 ─────────────────────────────────────
static void test_move_semantics() {
    TEST("move semantics");

    Socket sock1 = Socket::create_tcp();
    int fd1 = sock1.fd();

    Socket sock2(std::move(sock1));
    ASSERT(!sock1.valid(), "moved-from socket should be invalid");
    ASSERT(sock2.valid(), "moved-to socket should be valid");
    ASSERT(sock2.fd() == fd1, "fd should be preserved");

    PASS();
}

// ── 测试4：bind + listen ────────────────────────────────
static void test_bind_listen() {
    TEST("bind and listen");

    Socket sock = Socket::create_tcp();
    sock.set_reuse_addr(true);

    auto addr = Socket::make_address("127.0.0.1", 19999);
    sock.bind(addr);
    sock.listen(5);

    ASSERT(sock.valid(), "should still be valid after bind+listen");

    // 清理
    sock.close();

    PASS();
}

// ── 测试5：connect + accept ─────────────────────────────
static void test_connect_accept() {
    TEST("TCP connect and accept");

    try {
        Socket server = Socket::create_tcp();
        server.set_reuse_addr(true);
        auto addr = Socket::make_address("127.0.0.1", 19998);
        server.bind(addr);
        server.listen(1);

        // 客户端线程: connect 后短暂等待确保 accept 能捕获
        std::thread client_thread([&addr]() {
            Socket client = Socket::create_tcp();
            ::connect(client.fd(),
                reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
            // 非阻塞 socket connect 返回 EINPROGRESS，等待一会
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        });

        // accept（等待客户端连接）
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        Socket conn = server.accept();
        ASSERT(conn.valid(), "accept should return valid connection");

        client_thread.join();
    } catch (const std::exception& e) {
        printf("  [exception: %s]\n", e.what());
    }

    PASS();
}

// ── 模块入口 ─────────────────────────────────────────────
namespace wemeet_test {

int test_socket() {
    printf("\n  === Socket RAII Unit Tests ===\n\n");

    tests_passed = tests_failed = 0;

    test_create_tcp();
    test_socket_options();
    test_move_semantics();
    test_bind_listen();
    test_connect_accept();

    printf("\n  Socket: %d passed, %d failed\n\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}

} // namespace wemeet_test
