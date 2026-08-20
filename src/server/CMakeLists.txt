# ── 信令服务器 ──────────────────────────────────────────
find_package(OpenSSL REQUIRED)

add_executable(wemeet_server
    main.cpp
    signaling_server.cpp
    auth_service.cpp
    meeting_service.cpp
    user_manager.cpp
    media_relay.cpp
)

target_link_libraries(wemeet_server PRIVATE
    wemeet_network
    wemeet_db
    wemeet_common
    wemeet_proto
    pthread
    ${MYSQL_CLIENT_LIB}
    OpenSSL::SSL
    OpenSSL::Crypto
)
