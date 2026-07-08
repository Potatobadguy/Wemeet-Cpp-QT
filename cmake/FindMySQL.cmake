# ── 查找 MySQL 客户端库 ─────────────────────────────────
# 在系统中定位 libmysqlclient，设置以下变量：
#   MYSQL_CLIENT_FOUND  — 是否找到
#   MYSQL_CLIENT_INCLUDE_DIR
#   MYSQL_CLIENT_LIBRARY

find_path(MYSQL_CLIENT_INCLUDE_DIR
    NAMES mysql/mysql.h
    PATHS /usr/include /usr/local/include /usr/include/mysql
    PATH_SUFFIXES mysql
)

find_library(MYSQL_CLIENT_LIBRARY
    NAMES mysqlclient libmysqlclient
    PATHS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MySQL
    REQUIRED_VARS MYSQL_CLIENT_LIBRARY MYSQL_CLIENT_INCLUDE_DIR
)

if(MYSQL_CLIENT_FOUND)
    message(STATUS "Found MySQL client: ${MYSQL_CLIENT_LIBRARY}")
endif()
