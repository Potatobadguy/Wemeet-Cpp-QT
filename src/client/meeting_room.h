#pragma once
#include <QWidget>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <vector>

/**
 * @brief 会议室界面 — 视频画廊 + 控制栏 + 聊天面板
 *
 * 技术亮点：
 *   - QGridLayout 自适应画廊布局
 *   - 自定义视频控件模拟（无真实摄像头时显示头像占位）
 *   - 控制栏：静音/视频开关/屏幕共享/挂断
 */
class MeetingRoom : public QWidget {
    Q_OBJECT
public:
    explicit MeetingRoom(QWidget* parent = nullptr);

    void set_room_info(const QString& room_id, const QString& title);
    void add_participant(uint64_t user_id, const QString& nickname);
    void remove_participant(uint64_t user_id);

signals:
    void leave_meeting();

private slots:
    void on_mute_toggled();
    void on_video_toggled();
    void on_hangup();
    void on_send_chat();

private:
    void setup_video_gallery();
    void setup_control_bar();
    void setup_chat_panel();
    void update_gallery_layout();

    struct VideoTile {
        QFrame*     frame;
        QLabel*     label;
        QLabel*     name_label;
        uint64_t    user_id;
        bool        video_on = true;
    };

    // ── UI 元素 ───────────────────────────────────────────
    QGridLayout* gallery_layout_;
    QLabel*      room_title_;

    // ── 控制栏 ───────────────────────────────────────────
    QPushButton* mute_btn_;
    QPushButton* video_btn_;
    QPushButton* share_btn_;
    QPushButton* hangup_btn_;
    bool mic_muted_  = false;
    bool video_off_  = false;

    // ── 聊天 ─────────────────────────────────────────────
    QTextEdit*   chat_display_;
    QLineEdit*   chat_input_;
    QPushButton* send_btn_;

    // ── 参与者 ───────────────────────────────────────────
    std::vector<VideoTile> participants_;
};
