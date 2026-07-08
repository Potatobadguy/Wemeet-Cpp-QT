# ── 通用工具库 ──────────────────────────────────────────
add_library(wemeet_common STATIC
    memory_pool.cpp
    thread_pool.cpp
    logger.cpp
)

target_include_directories(wemeet_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wemeet_common PUBLIC
    wemeet_proto
    pthread
)
set_target_properties(wemeet_common PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
