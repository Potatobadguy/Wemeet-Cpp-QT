cmake_minimum_required(VERSION 3.20)
project(WeMeet VERSION 2.0.0 LANGUAGES CXX)

# ── C++17 标准 ──────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── 编译选项 ────────────────────────────────────────────
option(BUILD_TESTS   "Build unit tests"   ON)
option(BUILD_CLIENT  "Build Qt client"    ON)
option(BUILD_SERVER  "Build server"       ON)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0 -Wall -Wextra)
    # 如需 ASan 取消注释:
    # add_compile_options(-fsanitize=address)
    # add_link_options(-fsanitize=address)
else()
    add_compile_options(-O2 -DNDEBUG)
endif()

# ── 依赖查找 ────────────────────────────────────────────
find_package(Protobuf REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)

# MySQL 客户端库
find_library(MYSQL_CLIENT_LIB NAMES mysqlclient PATHS /usr/lib/x86_64-linux-gnu)
if(NOT MYSQL_CLIENT_LIB)
    message(STATUS "libmysqlclient not found — DB module will be compiled as stub")
endif()

# ── 子系统 ─────────────────────────────────────────────
add_subdirectory(proto)
add_subdirectory(src/common)

# 网络与数据库子系统（构建客户端或服务器时需要）
if(BUILD_CLIENT OR BUILD_SERVER)
    set(BUILD_NETWORK ON CACHE BOOL "" FORCE)
    add_subdirectory(src/network)

    if(MYSQL_CLIENT_LIB)
        set(BUILD_DB ON CACHE BOOL "" FORCE)
        add_subdirectory(src/db)
    endif()
endif()

if(BUILD_SERVER)
    add_subdirectory(src/server)
endif()

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(BUILD_CLIENT)
    find_package(Qt6 COMPONENTS Core Widgets Network Multimedia MultimediaWidgets REQUIRED)
    add_subdirectory(src/client)
endif()

message(STATUS "WeMeet v${PROJECT_VERSION}")
message(STATUS "  BUILD_CLIENT=${BUILD_CLIENT}  BUILD_SERVER=${BUILD_SERVER}")
message(STATUS "  BUILD_NETWORK=${BUILD_NETWORK}  BUILD_DB=${BUILD_DB}  BUILD_TESTS=${BUILD_TESTS}")
