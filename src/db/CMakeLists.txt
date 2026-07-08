# ── 数据库层 ────────────────────────────────────────────
add_library(wemeet_db STATIC
    connection_pool.cpp
    user_repository.cpp
    meeting_repository.cpp
)

target_include_directories(wemeet_db
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_SOURCE_DIR}/src/common
)

target_link_libraries(wemeet_db PUBLIC
    wemeet_common
    ${MYSQL_CLIENT_LIB}
    pthread
)
set_target_properties(wemeet_db PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
