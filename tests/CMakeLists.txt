# ── 单元测试 ────────────────────────────────────────────
add_executable(wemeet_test
    test_main.cpp
    test_memory_pool.cpp
    test_lockfree_queue.cpp
    test_thread_pool.cpp
    test_buffer.cpp
    test_event_loop.cpp
    test_socket.cpp
    test_codec.cpp
)

target_link_libraries(wemeet_test PRIVATE
    wemeet_common
    wemeet_network
    wemeet_proto
)

target_include_directories(wemeet_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/network
)

add_test(NAME wemeet_test COMMAND wemeet_test)
