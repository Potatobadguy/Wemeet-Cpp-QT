/**
 * @brief WeMeet 单元测试总入口
 *
 * 所有测试套件汇总于此，一个 main() 驱动全部测试。
 */

// 各测试模块入口
namespace wemeet_test {
    int test_memory_pool();
    int test_lockfree_queue();
    int test_thread_pool();
    int test_buffer();
    int test_event_loop();
    int test_socket();
    int test_codec();
}

#include <cstdio>

int main() {
    printf("\n");
    printf("╔══════════════════════════════════════╗\n");
    printf("║     WeMeet Unit Test Suite           ║\n");
    printf("║     C++17 | Protobuf | Boost         ║\n");
    printf("╚══════════════════════════════════════╝\n");

    int failed = 0;

    failed += wemeet_test::test_memory_pool();
    failed += wemeet_test::test_lockfree_queue();
    failed += wemeet_test::test_thread_pool();
    failed += wemeet_test::test_buffer();
    failed += wemeet_test::test_event_loop();
    failed += wemeet_test::test_socket();
    failed += wemeet_test::test_codec();

    printf("\n");
    if (failed == 0) {
        printf("═══ All test suites PASSED ═══\n\n");
    } else {
        printf("═══ %d test suite(s) FAILED ═══\n\n", failed);
    }

    return failed > 0 ? 1 : 0;
}
