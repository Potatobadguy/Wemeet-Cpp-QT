#pragma once
/**
 * @file share_source_dialog.h
 * @brief 屏幕共享源选择对话框（#19）
 *
 * 交互设计：
 *   - 双 Tab：「整个屏幕」（QScreen 缩略图）/「应用窗口」（WindowEnumerator）；
 *   - 每 Tab 内为 3 列缩略图网格（QListWidget IconMode），单选高亮；
 *   - 双击卡片直接开始共享；「开始共享」按钮在未选中任何源时置灰；
 *   - Linux 等不支持窗口枚举的平台，窗口 Tab 显示降级提示。
 */

#include <QDialog>
#include <QTabWidget>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include "window_enumerator.h"   // ShareSource / WindowInfo

class ShareSourceDialog : public QDialog {
    Q_OBJECT
public:
    explicit ShareSourceDialog(QWidget* parent = nullptr);
    ~ShareSourceDialog() override = default;

    /**
     * @brief 当前选中的共享源（对话框 accept 后调用有效）
     */
    ShareSource selected_source() const { return selected_; }

signals:
    /**
     * @brief 双击卡片时直接发出（对话框同时 accept）
     */
    void source_selected(const ShareSource& source);

private slots:
    void on_selection_changed();
    void on_item_double_clicked(QListWidgetItem* item);
    void on_start_clicked();

private:
    void setup_ui();
    void populate_screens();    // 屏幕 Tab：QScreen 缩略图
    void populate_windows();    // 窗口 Tab：WindowEnumerator 结果
    QListWidget* current_list() const;
    void update_start_button();

    // ── UI ───────────────────────────────────────────────
    QTabWidget*   tabs_ = nullptr;
    QListWidget*  screen_list_ = nullptr;   // 屏幕网格（item data 存 screen_index）
    QListWidget*  window_list_ = nullptr;   // 窗口网格（item data 存 window_id）
    QPushButton*  start_btn_ = nullptr;
    QPushButton*  cancel_btn_ = nullptr;

    ShareSource   selected_;                // 当前选中源

    static constexpr int kGridColumns = 3;          // 3 列缩略图网格
    static constexpr QSize kThumbSize{240, 150};    // 卡片缩略图尺寸
};
