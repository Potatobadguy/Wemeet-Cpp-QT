#pragma once
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QQueue>
#include <functional>
#include <cstdint>

/**
 * @brief 异步网络客户端 — QTcpSocket + 信号驱动
 *
 * 技术亮点：
 *   - QTcpSocket 非阻塞 I/O
 *   - 信号槽连接网络事件到 UI 更新
 *   - 自动重连机制
 *   - Protobuf 协议编解码
 */
class NetworkClient : public QObject {
    Q_OBJECT
public:
    explicit NetworkClient(QObject* parent = nullptr);

    void connect_to_server(const QString& host, uint16_t port);
    void disconnect();

    // 发送 Protobuf 消息
    void send_message(const std::string& serialized_data);

    bool is_connected() const { return socket_->state() == QAbstractSocket::ConnectedState; }

signals:
    void connected();
    void disconnected();
    void message_received(const std::string& data);
    void error_occurred(const QString& error);

private slots:
    void on_connected();
    void on_disconnected();
    void on_ready_read();
    void on_error(QAbstractSocket::SocketError err);
    void on_reconnect();

private:
    QTcpSocket* socket_;
    QTimer*     reconnect_timer_;
    QString     host_;
    uint16_t    port_     = 0;
    int         reconnect_attempts_ = 0;

    // 接收缓冲区（处理粘包）
    QByteArray  recv_buffer_;
    static constexpr int kMaxReconnectAttempts = 5;
    static constexpr int kReconnectIntervalMs  = 3000;

    // 发送队列（未连接时缓存）
    QQueue<QByteArray> send_queue_;
    static constexpr int kMaxQueueSize = 100;
};
