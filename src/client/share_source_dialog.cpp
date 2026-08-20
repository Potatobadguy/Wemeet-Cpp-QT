#include "share_source_dialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGuiApplication>
#include <QScreen>
#include <QListWidgetItem>
#include <QPainter>
#include <QColor>
#include <QFont>

ShareSourceDialog::ShareSourceDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("选择共享内容"));
    setMinimumSize(820, 520);
    setup_ui();
    populate_screens();
    populate_windows();
    update_start_button();
}

void ShareSourceDialog::setup_ui() {
    auto* main_layout = new QVBoxLayout(this);

    tabs_ = new QTabWidget(this);

    // ── 屏幕 Tab：3 列图标网格 ──
    screen_list_ = new QListWidget(tabs_);
    screen_list_->setViewMode(QListWidget::IconMode);
    screen_list_->setResizeMode(QListWidget::Adjust);
    screen_list_->setGridSize(QSize(kThumbSize.width() + 40, kThumbSize.height() + 50));
    screen_list_->setIconSize(kThumbSize);
    screen_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    screen_list_->setSpacing(12);
    screen_list_->setWordWrap(true);
    tabs_->addTab(screen_list_, QStringLiteral("整个屏幕"));

    // ── 窗口 Tab：3 列图标网格 ──
    window_list_ = new QListWidget(tabs_);
    window_list_->setViewMode(QListWidget::IconMode);
    window_list_->setResizeMode(QListWidget::Adjust);
    window_list_->setGridSize(QSize(kThumbSize.width() + 40, kThumbSize.height() + 50));
    window_list_->setIconSize(kThumbSize);
    window_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    window_list_->setSpacing(12);
    window_list_->setWordWrap(true);
    tabs_->addTab(window_list_, QStringLiteral("应用窗口"));

    main_layout->addWidget(tabs_);

    // ── 底部按钮 ──
    auto* btn_layout = new QHBoxLayout();
    btn_layout->addStretch();
    cancel_btn_ = new QPushButton(QStringLiteral("取消"), this);
    start_btn_  = new QPushButton(QStringLiteral("开始共享"), this);
    start_btn_->setDefault(true);
    start_btn_->setEnabled(false);   // 未选中时置灰（#19）
    btn_layout->addWidget(cancel_btn_);
    btn_layout->addWidget(start_btn_);
    main_layout->addLayout(btn_layout);

    // ── 信号连接 ──
    connect(cancel_btn_, &QPushButton::clicked, this, &QDialog::reject);
    connect(start_btn_,  &QPushButton::clicked, this, &ShareSourceDialog::on_start_clicked);

    for (auto* list : {screen_list_, window_list_}) {
        connect(list, &QListWidget::itemSelectionChanged,
                this, &ShareSourceDialog::on_selection_changed);
        connect(list, &QListWidget::itemDoubleClicked,
                this, &ShareSourceDialog::on_item_double_clicked);
    }

    // 切换 Tab 时清空另一 Tab 的选中，保证"单源"语义
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == 0) {
            window_list_->clearSelection();
        } else {
            screen_list_->clearSelection();
        }
        on_selection_changed();
    });
}

void ShareSourceDialog::populate_screens() {
    const QList<QScreen*> screens = QGuiApplication::screens();
    for (int i = 0; i < screens.size(); ++i) {
        QScreen* s = screens[i];
        // WSLg/部分合成器下 grabWindow(0) 可能返回空 pixmap 或全黑 pixmap
        QPixmap shot = s->grabWindow(0);

        // ★ 检测"全黑 pixmap"：取一个 4x4 缩略图计算平均亮度，若接近全黑
        //    视为抓取失败（WSLg Wayland 抓屏返回的非空但全黑的 pixmap）
        bool capture_failed = shot.isNull();
        if (!capture_failed) {
            QPixmap probe = shot.scaled(4, 4, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QImage img = probe.toImage().convertToFormat(QImage::Format_RGB32);
            quint64 sum = 0;
            for (int y = 0; y < img.height(); ++y) {
                const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
                for (int x = 0; x < img.width(); ++x) sum += qGray(line[x]);
            }
            int pixels = img.width() * img.height();
            int avg = pixels > 0 ? static_cast<int>(sum / pixels) : 0;
            // 平均亮度 < 10 视为抓取失败（全黑或接近全黑）
            capture_failed = (avg < 10);
        }

        if (capture_failed) {
            // 兜底：构造一个带文字占位的灰色 pixmap（替代黑色块）
            shot = QPixmap(s->size().width(), s->size().height());
            shot.fill(QColor(40, 50, 70));
            QPainter p(&shot);
            p.setPen(QColor(180, 180, 200));
            p.setFont(QFont("Microsoft YaHei", 24, QFont::Bold));
            p.drawText(shot.rect(), Qt::AlignCenter,
                       QStringLiteral("屏幕 %1\n（缩略图不可用）").arg(i + 1));
            p.end();
        }
        QPixmap thumb = shot.scaled(
            kThumbSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);

        QString title = QStringLiteral("屏幕 %1 (%2×%3)")
                            .arg(i + 1)
                            .arg(s->size().width())
                            .arg(s->size().height());
        auto* item = new QListWidgetItem(QIcon(thumb), title, screen_list_);
        item->setData(Qt::UserRole, i);   // screen_index
        item->setTextAlignment(Qt::AlignHCenter);
        screen_list_->addItem(item);
    }
}

void ShareSourceDialog::populate_windows() {
    const QList<WindowInfo> windows = WindowEnumerator::enumerate_windows();

    if (windows.isEmpty()) {
        // 平台降级（Linux 等）：提示仅支持整屏共享
        auto* item = new QListWidgetItem(
            QIcon(), QStringLiteral("当前平台不支持窗口级共享，请使用「整个屏幕」"),
            window_list_);
        item->setFlags(Qt::NoItemFlags);   // 不可选中
        window_list_->addItem(item);
        return;
    }

    for (const WindowInfo& w : windows) {
        QPixmap thumb = w.thumbnail.isNull()
            ? QPixmap(kThumbSize)   // 无缩略图时占位
            : w.thumbnail;
        if (thumb.isNull()) {
            thumb = QPixmap(kThumbSize);
            thumb.fill(QColor(0x22, 0x2b, 0x45));
        }
        auto* item = new QListWidgetItem(QIcon(thumb), w.title, window_list_);
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(
                        static_cast<qulonglong>(w.id)));   // window_id
        item->setToolTip(w.title);
        item->setTextAlignment(Qt::AlignHCenter);
        window_list_->addItem(item);
    }
}

QListWidget* ShareSourceDialog::current_list() const {
    return tabs_->currentIndex() == 0
               ? static_cast<QListWidget*>(screen_list_)
               : static_cast<QListWidget*>(window_list_);
}

void ShareSourceDialog::on_selection_changed() {
    QListWidget* list = current_list();
    auto items = list->selectedItems();
    if (items.isEmpty()) {
        update_start_button();
        return;
    }

    QListWidgetItem* item = items.first();
    if (list == screen_list_) {
        selected_.type = ShareSource::Type::SCREEN;
        selected_.screen_index = item->data(Qt::UserRole).toInt();
        selected_.window_id = 0;
        selected_.title = item->text();
    } else {
        selected_.type = ShareSource::Type::WINDOW;
        selected_.window_id = static_cast<uintptr_t>(
            item->data(Qt::UserRole).toULongLong());
        selected_.screen_index = 0;
        selected_.title = item->text();
    }
    update_start_button();
}

void ShareSourceDialog::on_item_double_clicked(QListWidgetItem* /*item*/) {
    // 双击直接开始（等价于选中 + 点开始）
    on_selection_changed();
    if (start_btn_->isEnabled()) {
        emit source_selected(selected_);
        accept();
    }
}

void ShareSourceDialog::on_start_clicked() {
    on_selection_changed();   // 兜底同步选中状态
    if (!start_btn_->isEnabled()) return;
    emit source_selected(selected_);
    accept();
}

void ShareSourceDialog::update_start_button() {
    bool has_valid = false;
    QListWidget* list = current_list();
    auto items = list->selectedItems();
    if (!items.isEmpty() && (items.first()->flags() & Qt::ItemIsSelectable)) {
        has_valid = true;
    }
    start_btn_->setEnabled(has_valid);
}
