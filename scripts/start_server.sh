#!/bin/bash
# ═══════════════════════════════════════════════════════════
#  WeMeet — 一键编译、测试、启动脚本
#  用法:
#    ./scripts/start_server.sh              # 编译 + 测试
#    ./scripts/start_server.sh --run        # 编译 + 测试 + 启动服务器
#    ./scripts/start_server.sh --port 8080  # 指定端口启动
# ═══════════════════════════════════════════════════════════

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"

# ── 颜色输出 ─────────────────────────────────────────────
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; }

# ── 默认参数 ─────────────────────────────────────────────
RUN_SERVER=false
SERVER_PORT=9090
SERVER_IP="0.0.0.0"
DB_HOST="127.0.0.1"
DB_PORT=3306
DB_USER="root"
DB_PASS=""
DB_NAME="wemeet"

# ── 解析参数 ─────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case $1 in
        --run)    RUN_SERVER=true; shift ;;
        --port)   SERVER_PORT="$2"; shift 2 ;;
        --ip)     SERVER_IP="$2";   shift 2 ;;
        --db-host) DB_HOST="$2";    shift 2 ;;
        --db-port) DB_PORT="$2";    shift 2 ;;
        --db-user) DB_USER="$2";    shift 2 ;;
        --db-pass) DB_PASS="$2";    shift 2 ;;
        --db-name) DB_NAME="$2";    shift 2 ;;
        --help|-h)
            echo "WeMeet 一键编译启动脚本"
            echo ""
            echo "用法: $0 [选项]"
            echo ""
            echo "选项:"
            echo "  --run          编译测试通过后启动服务器"
            echo "  --port PORT    服务监听端口 (默认: 9090)"
            echo "  --ip IP        服务监听地址 (默认: 0.0.0.0)"
            echo "  --db-host HOST MySQL 地址 (默认: 127.0.0.1)"
            echo "  --db-port PORT MySQL 端口 (默认: 3306)"
            echo "  --db-user USER MySQL 用户 (默认: root)"
            echo "  --db-pass PASS MySQL 密码"
            echo "  --db-name NAME 数据库名 (默认: wemeet)"
            echo "  --help, -h     显示帮助"
            exit 0
            ;;
        *) error "未知参数: $1"; exit 1 ;;
    esac
done

echo ""
echo "╔══════════════════════════════════════╗"
echo "║     WeMeet Build & Start Script      ║"
echo "║     C++17 | Qt6 | Protobuf | MySQL   ║"
echo "╚══════════════════════════════════════╝"
echo ""

# ── 检查依赖 ─────────────────────────────────────────────
info "检查编译依赖..."
MISSING=""
for cmd in g++ cmake protoc; do
    if ! command -v "$cmd" &>/dev/null; then
        MISSING="$MISSING $cmd"
    fi
done

if [ -n "$MISSING" ]; then
    error "缺少依赖:$MISSING"
    error "请运行: sudo apt install -y build-essential cmake libprotobuf-dev protobuf-compiler libboost-system-dev libmysqlclient-dev libssl-dev qt6-base-dev"
    exit 1
fi
info "依赖检查通过"

# ── 编译 ─────────────────────────────────────────────────
info "CMake 配置中..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake "$PROJECT_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SERVER=ON \
    -DBUILD_NETWORK=ON \
    -DBUILD_DB=ON \
    -DBUILD_TESTS=ON \
    -DBUILD_CLIENT=OFF \
    > /dev/null

info "编译中 (使用 $(nproc) 线程)..."
cmake --build . -j"$(nproc)" 2>&1 | tail -5

# ── 测试 ─────────────────────────────────────────────────
info "运行单元测试..."
if stdbuf -oL timeout 15 ./tests/wemeet_test 2>&1; then
    info "全部测试通过!"
else
    error "测试失败, 请检查编译输出"
    exit 1
fi

# ── 启动服务器 ───────────────────────────────────────────
if [ "$RUN_SERVER" = true ]; then
    echo ""
    info "启动信令服务器: ${SERVER_IP}:${SERVER_PORT}"
    info "数据库: ${DB_USER}@${DB_HOST}:${DB_PORT}/${DB_NAME}"
    echo ""
    exec ./src/server/wemeet_server \
        --ip "$SERVER_IP" \
        --port "$SERVER_PORT" \
        --db-host "$DB_HOST" \
        --db-port "$DB_PORT" \
        --db-user "$DB_USER" \
        --db-pass "$DB_PASS" \
        --db-name "$DB_NAME"
else
    echo ""
    info "编译测试完成! 手动启动服务器:"
    echo ""
    echo "  ./build/src/server/wemeet_server --port ${SERVER_PORT}"
    echo ""
    echo "  或使用: $0 --run"
    echo ""
fi
