cmake_minimum_required(VERSION 3.20)
project(WeMeet VERSION 1.0.0 LANGUAGES CXX)

# ── C++17 标准 ──────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── 编译选项 ────────────────────────────────────────────
option(BUILD_TESTS   "Build unit tests"   ON)
option(BUILD_CLIENT  "Build Qt client"    OFF)   # Qt 未安装时关闭
option(BUILD_SERVER  "Build server"       ON)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0 -Wall -Wextra -fsanitize=address)
    add_link_options(-fsanitize=address)
else()
    add_compile_options(-O2 -DNDEBUG)
endif()

# ── 依赖查找 ────────────────────────────────────────────
find_package(Protobuf REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)

# MySQL 客户端库（内置在 protobuf 安装中已包含或系统自带）
find_library(MYSQL_CLIENT_LIB NAMES mysqlclient PATHS /usr/lib/x86_64-linux-gnu)
if(NOT MYSQL_CLIENT_LIB)
    message(STATUS "libmysqlclient not found — DB module will be compiled as stub")
endif()

# ── 子系统（按 Phase 逐步启用）────────────────────────
add_subdirectory(proto)
add_subdirectory(src/common)

option(BUILD_NETWORK "Build network framework" OFF)
option(BUILD_DB      "Build database layer"   OFF)

if(BUILD_NETWORK)
    add_subdirectory(src/network)
endif()

if(BUILD_DB)
    add_subdirectory(src/db)
endif()

if(BUILD_SERVER)
    add_subdirectory(src/server)
endif()

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(BUILD_CLIENT)
    find_package(Qt6 COMPONENTS Core Widgets Network Multimedia REQUIRED)
    add_subdirectory(src/client)
endif()
