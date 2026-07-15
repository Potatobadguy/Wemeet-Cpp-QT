#pragma once
#include <QWidget>
#include <QGridLayout>
#include <QStackedLayout>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QSplitter>
#include <QScrollArea>
#include <QStackedWidget>
#include <QComboBox>
#include <QSlider>
#include <QProgressBar>
#include <QMenu>
#include <QListWidget>
#include <QListWidgetItem>
#include <QVideoWidget>
#include <memory>
#include <vector>
#include <unordered_map>

class RtpSession;
class MediaEngine;
class RemoteVideoWidget;

/**
 * @brief 增强会议室界面 — 实时视频画廊 + 控制栏 + 聊天 + 自适应布局
 *
 * 技术亮点：
 *   - MediaEngine 驱动的实时音视频传输
 *   - 自适应网格布局（桌面/移动端）
 *   - 屏幕共享集成
 *   - 网络质量指示
 *   - 成员列表与权限管理
 */
class MeetingRoom : public QWidget {
    Q_OBJECT
public:
    explicit MeetingRoom(QWidget* parent = nullptr);
    ~MeetingRoom();

    void set_room_info(const QString& room_id, const QString& title,
                       uint64_t local_user_id, const QString& nickname);
    void set_media_engine(MediaEngine* engine);
    MediaEngine* media_engine() const { return media_engine_; }

    // 参与者管理
    void add_participant(uint64_t user_id, const QString& nickname,
                         bool is_local = false, bool audio_on = true,
                         bool video_on = true, bool is_host = false);
    void remove_participant(uint64_t user_id);
    void update_participant(uint64_t user_id, bool audio_on, bool video_on);

    // 屏幕共享
    void on_screen_share_started(uint64_t user_id, const QString& nickname);
    void on_screen_share_stopped(uint64_t user_id);

    // 网络质量
    void update_network_quality(int32_t quality);

signals:
    void leave_meeting();
    void back_to_lobby();
    void mute_toggled(bool muted);
    void video_toggled(bool off);
    void screen_share_toggled(bool sharing);
    void role_changed(uint64_t target_user_id, int role);

    // 通过信令发送的消息
    void send_signal_message(const std::string& data);
    void send_chat_text(const QString& content);
    void send_leave_meeting();

public slots:
    void append_chat_message(uint64_t user_id, const QString& nickname, const QString& content);

private slots:
    void on_mute_toggled();
    void on_video_toggled();
    void on_share_toggled();
    void on_hangup();
    void on_back();
    void on_send_chat();

    // 参与者和角色管理
    void on_participant_context_menu(uint64_t user_id);

    // 音量更新
    void on_volume_level_changed(double level);

private:
    void setup_ui();

    // 画廊布局
    struct VideoTile {
        QFrame*     frame;
        RemoteVideoWidget* video_widget;
        QLabel*     avatar_label;
        QLabel*     name_label;
        QLabel*     quality_icon;
        QPushButton* mute_indicator;
        uint64_t    user_id;
        QString     nickname;
        bool        video_on = true;
        bool        audio_on = true;
        bool        is_local = false;
        bool        is_host  = false;
    };

    void rebuild_gallery();
    VideoTile* find_tile(uint64_t user_id);
    int  gallery_columns() const;
    void apply_tile_style(VideoTile& tile);
    void rebuild_member_list();
    void update_member_list();

    // ── UI 布局 ───────────────────────────────────────────
    QVBoxLayout*   main_layout_;
    QLabel*        room_title_;
    QGridLayout*   gallery_layout_;
    QWidget*       gallery_container_;
    QScrollArea*   gallery_scroll_;
    QStackedWidget* content_stack_;

    // ── 控制栏 ───────────────────────────────────────────
    QWidget*     control_bar_;
    QPushButton* mute_btn_;
    QProgressBar* volume_bar_;   // 麦克风音量指示条
    QPushButton* video_btn_;
    QPushButton* share_btn_;
    QPushButton* members_btn_;
    QPushButton* back_btn_;
    QPushButton* hangup_btn_;
    bool mic_muted_  = false;
    bool video_off_  = false;
    bool sharing_    = false;

    // ── 聊天面板 ─────────────────────────────────────────
    QTextEdit*   chat_display_;
    QLineEdit*   chat_input_;
    QPushButton* send_btn_;
    QWidget*     chat_panel_;

    // ── 成员面板 ─────────────────────────────────────────
    QListWidget* member_list_;
    QWidget*     member_panel_;

    // ── 网络质量指示 ─────────────────────────────────────
    QLabel*      quality_indicator_;
    QLabel*      participant_count_label_;
    int          network_quality_ = 5;

    // ── 参与者 ───────────────────────────────────────────
    std::vector<VideoTile> participants_;
    uint64_t local_user_id_ = 0;
    QString  nickname_;
    QString  room_id_;
    QString  room_title_str_;
    bool     is_host_ = false;

    // ── 媒体引擎 ─────────────────────────────────────────
    MediaEngine* media_engine_ = nullptr;

    // ── 桌面/移动检测 ────────────────────────────────────
    static constexpr int kMobileWidth = 768;
    bool is_mobile_layout() const { return width() < kMobileWidth; }

protected:
    void resizeEvent(QResizeEvent* event) override;
};
