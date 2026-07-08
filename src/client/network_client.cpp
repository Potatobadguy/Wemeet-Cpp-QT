#include "network_client.h"
#include <QDebug>
#include <QtEndian>

NetworkClient::NetworkClient(QObject* parent)
    : QObject(parent)
    , socket_(new QTcpSocket(this))
    , reconnect_timer_(new QTimer(this)) {

    reconnect_timer_->setInterval(kReconnectIntervalMs);
    reconnect_timer_->setSingleShot(true);

    // ── 信号槽连接 ───────────────────────────────────────
    connect(socket_, &QTcpSocket::connected,
            this, &NetworkClient::on_connected);
    connect(socket_, &QTcpSocket::disconnected,
            this, &NetworkClient::on_disconnected);
    connect(socket_, &QTcpSocket::readyRead,
            this, &NetworkClient::on_ready_read);
    connect(socket_, &QTcpSocket::errorOccurred,
            this, &NetworkClient::on_error);
    connect(reconnect_timer_, &QTimer::timeout,
            this, &NetworkClient::on_reconnect);
}

void NetworkClient::connect_to_server(const QString& host, uint16_t port) {
    host_ = host;
    port_ = port;
    reconnect_attempts_ = 0;
    socket_->connectToHost(host, port);
}

void NetworkClient::disconnect() {
    reconnect_timer_->stop();
    socket_->disconnectFromHost();
}

void NetworkClient::send_message(const std::string& data) {
    if (!is_connected()) return;

    // 4字节大端长度头 + Protobuf数据
    uint32_t len = static_cast<uint32_t>(data.size());
    uint32_t net_len = qToBigEndian(len);

    QByteArray packet;
    packet.append(reinterpret_cast<const char*>(&net_len), 4);
    packet.append(data.data(), data.size());

    socket_->write(packet);
    socket_->flush();
}

// ── 槽函数 ───────────────────────────────────────────────
void NetworkClient::on_connected() {
    reconnect_attempts_ = 0;
    qDebug() << "NetworkClient: connected to" << host_ << ":" << port_;
    emit connected();
}

void NetworkClient::on_disconnected() {
    qDebug() << "NetworkClient: disconnected";
    emit disconnected();

    // 自动重连
    if (reconnect_attempts_ < kMaxReconnectAttempts) {
        reconnect_timer_->start();
    }
}

void NetworkClient::on_ready_read() {
    recv_buffer_.append(socket_->readAll());

    // 处理粘包: 4字节长度头
    while (recv_buffer_.size() >= 4) {
        uint32_t body_len = qFromBigEndian(
            *reinterpret_cast<const uint32_t*>(recv_buffer_.constData()));

        if (body_len > 64 * 1024 * 1024) {  // 64MB 上限
            qWarning() << "NetworkClient: message too large:" << body_len;
            socket_->close();
            return;
        }

        if (recv_buffer_.size() < 4 + static_cast<int>(body_len)) break;  // 数据不完整

        // 提取消息
        QByteArray body = recv_buffer_.mid(4, body_len);
        recv_buffer_.remove(0, 4 + body_len);

        emit message_received(body.toStdString());
    }
}

void NetworkClient::on_error(QAbstractSocket::SocketError err) {
    Q_UNUSED(err);
    qWarning() << "NetworkClient: error" << socket_->errorString();
    emit error_occurred(socket_->errorString());
}

void NetworkClient::on_reconnect() {
    reconnect_attempts_++;
    qDebug() << "NetworkClient: reconnect attempt" << reconnect_attempts_;
    socket_->connectToHost(host_, port_);
}
