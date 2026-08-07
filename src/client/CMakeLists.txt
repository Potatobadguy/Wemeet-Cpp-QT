# ── Qt 客户端 ───────────────────────────────────────────
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Multimedia MultimediaWidgets)

qt_add_executable(wemeet_client
    main.cpp
    main_window.h main_window.cpp
    login_dialog.h login_dialog.cpp
    meeting_room.h meeting_room.cpp
    network_client.h network_client.cpp
    media_engine.h media_engine.cpp
    rtp_session.h rtp_session.cpp
    v4l2_capture.h v4l2_capture.cpp
    # ── 屏幕共享组件（T04）──
    window_enumerator.h window_enumerator.cpp
    share_source_dialog.h share_source_dialog.cpp
    screen_share_controller.h screen_share_controller.cpp
    share_toolbar.h share_toolbar.cpp
)

target_link_libraries(wemeet_client PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::Multimedia
    Qt6::MultimediaWidgets
    wemeet_proto
)

# Windows 下链接 dwmapi：窗口缩略图 / DWM 遮蔽窗口过滤（#19）
if(WIN32)
    target_link_libraries(wemeet_client PRIVATE dwmapi)
endif()

target_include_directories(wemeet_client PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/protobuf-build
)

# 确保 protobuf 生成的头文件可访问
target_include_directories(wemeet_client PRIVATE
    ${CMAKE_BINARY_DIR}/proto
)
