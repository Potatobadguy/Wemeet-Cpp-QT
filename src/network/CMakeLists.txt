# ── 网络框架库 ──────────────────────────────────────────
add_library(wemeet_network STATIC
    socket.cpp
    event_loop.cpp
    event_loop_pool.cpp
    tcp_server.cpp
    tcp_connection.cpp
)

target_include_directories(wemeet_network
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_SOURCE_DIR}/src/common
)
target_link_libraries(wemeet_network PUBLIC
    wemeet_common
    wemeet_proto
    pthread
)
set_target_properties(wemeet_network PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
