# ── Qt 客户端 ───────────────────────────────────────────
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Multimedia)

qt_add_executable(wemeet_client
    main.cpp
    main_window.h main_window.cpp
    login_dialog.h login_dialog.cpp
    meeting_room.h meeting_room.cpp
    network_client.h network_client.cpp
)

target_link_libraries(wemeet_client PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::Multimedia
    wemeet_proto
)

target_include_directories(wemeet_client PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/network
)
