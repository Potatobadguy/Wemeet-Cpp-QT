/**
 * @brief WeMeet 信令服务器入口
 *
 * 编译:
 *   cmake --build build --target wemeet_server
 *
 * 运行:
 *   ./build/src/server/wemeet_server --port 9090 --db-host 127.0.0.1
 */

#include "signaling_server.h"
#include "logger.h"
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <cstring>

using namespace wemeet;

// 全局指针用于信号处理
static SignalingServer* g_server = nullptr;

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        LOG_INFO("Received signal %d, shutting down...", sig);
        if (g_server) {
            g_server->stop();
        }
    }
}

int main(int argc, char* argv[]) {
    // ── 解析命令行参数 ───────────────────────────────────
    SignalingServer::Config config;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            config.listen_port = static_cast<uint16_t>(std::stoul(argv[++i]));
        } else if (arg == "--ip" && i + 1 < argc) {
            config.listen_ip = argv[++i];
        } else if (arg == "--threads" && i + 1 < argc) {
            config.worker_threads = std::stoul(argv[++i]);
        } else if (arg == "--db-host" && i + 1 < argc) {
            config.db_host = argv[++i];
        } else if (arg == "--db-port" && i + 1 < argc) {
            config.db_port = std::stoi(argv[++i]);
        } else if (arg == "--db-user" && i + 1 < argc) {
            config.db_user = argv[++i];
        } else if (arg == "--db-pass" && i + 1 < argc) {
            config.db_pass = argv[++i];
        } else if (arg == "--db-name" && i + 1 < argc) {
            config.db_name = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "WeMeet Signaling Server\n\n"
                      << "Usage: wemeet_server [options]\n\n"
                      << "Options:\n"
                      << "  --port PORT       Listen port (default: 9090)\n"
                      << "  --ip IP           Listen IP (default: 0.0.0.0)\n"
                      << "  --threads N       Worker threads (default: auto)\n"
                      << "  --db-host HOST    MySQL host (default: 127.0.0.1)\n"
                      << "  --db-port PORT    MySQL port (default: 3306)\n"
                      << "  --db-user USER    MySQL user (default: root)\n"
                      << "  --db-pass PASS    MySQL password (or env WEMEET_DB_PASS; required)\n"
                      << "  --db-name NAME    MySQL database (default: wemeet)\n"
                      << "  --help, -h        Show this help\n";
            return 0;
        }
    }

    // ── 初始化日志 ───────────────────────────────────────
    Logger::instance().set_level(LogLevel::INFO);
    Logger::instance().set_console(true);

    // ── 数据库密码校验（#14）─────────────────────────────
    // 优先级：命令行 --db-pass > 环境变量 WEMEET_DB_PASS；
    // 两者均为空则拒绝启动（禁止硬编码默认密码上线）。
    if (config.db_pass.empty()) {
        const char* env_pass = std::getenv("WEMEET_DB_PASS");
        if (env_pass && env_pass[0] != '\0') {
            config.db_pass = env_pass;
        }
    }
    if (config.db_pass.empty()) {
        LOG_FATAL("Database password is empty. Provide it via --db-pass "
                  "or the WEMEET_DB_PASS environment variable. Refusing to start.");
        std::cerr << "[FATAL] 数据库密码为空：请通过 --db-pass 参数或 "
                     "WEMEET_DB_PASS 环境变量提供，服务器拒绝启动。\n";
        return 1;
    }

    LOG_INFO("WeMeet Signaling Server v2.0.0");
    LOG_INFO("Listening on %s:%u", config.listen_ip.c_str(), config.listen_port);

    // ── 注册信号处理 ─────────────────────────────────────
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ── 启动服务器 ───────────────────────────────────────
    SignalingServer server(config);
    g_server = &server;
    server.start();

    LOG_INFO("Server exited cleanly");
    return 0;
}
