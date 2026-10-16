#include "ui/TitleBar.h"

#include "ui/AppIcon.h"

#include <QAction>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QToolButton>
#include <QWindow>

namespace ClipNote {
namespace {

constexpr int kBarHeight = 32;
constexpr int kButtonWidth = 34;
constexpr int kGlyphSize = 16;

// 图标用 QPainter 画，不用字形/emoji —— 那些在不同机器上渲染差别太大
QPixmap closeGlyph(int size, const QColor &color)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(color);
    pen.setWidthF(qMax(1.2, size / 9.0));
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);

    const qreal s = size;
    painter.drawLine(QPointF(s * 0.30, s * 0.30), QPointF(s * 0.70, s * 0.70));
    painter.drawLine(QPointF(s * 0.70, s * 0.30), QPointF(s * 0.30, s * 0.70));
    painter.end();

    return pixmap;
}

// 齿轮：一圈短齿 + 一个环。转着画八根齿比手算八个角度的坐标好读。
QPixmap gearGlyph(int size, const QColor &color)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal s = size;
    painter.translate(s / 2.0, s / 2.0);

    QPen pen(color);
    pen.setCapStyle(Qt::FlatCap);
    pen.setWidthF(s * 0.15);
    painter.setPen(pen);

    for (int i = 0; i < 8; ++i) {
        painter.save();
        painter.rotate(i * 45.0);
        painter.drawLine(QPointF(0, -s * 0.25), QPointF(0, -s * 0.41));
        painter.restore();
    }

    pen.setWidthF(s * 0.14);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(QPointF(0, 0), s * 0.26, s * 0.26);
    painter.end();

    return pixmap;
}

} // namespace

TitleBar::TitleBar(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("clipNoteTitleBar"));
    setFixedHeight(kBarHeight);
    setCursor(Qt::ArrowCursor);

    auto *icon = new QLabel(this);
    icon->setPixmap(appIcon().pixmap(kGlyphSize, kGlyphSize));
    icon->setFixedSize(kGlyphSize, kGlyphSize);

    m_title = new QLabel(QStringLiteral("ClipNote"), this);

    m_gear = new QToolButton(this);
    m_gear->setIcon(QIcon(gearGlyph(kGlyphSize, QColor(0x44, 0x44, 0x44))));
    m_gear->setIconSize(QSize(kGlyphSize, kGlyphSize));
    m_gear->setFixedSize(kButtonWidth, kBarHeight);
    m_gear->setToolTip(QStringLiteral("更多"));
    m_gear->setFocusPolicy(Qt::NoFocus);   // 面板收不到键盘，别让按钮去抢焦点
    m_gear->setCursor(Qt::ArrowCursor);
    connect(m_gear, &QToolButton::clicked, this, &TitleBar::showGearMenu);

    auto *close = new QToolButton(this);
    close->setIcon(QIcon(closeGlyph(kGlyphSize, QColor(0x44, 0x44, 0x44))));
    close->setIconSize(QSize(kGlyphSize, kGlyphSize));
    close->setFixedSize(kButtonWidth, kBarHeight);
    close->setToolTip(QStringLiteral("收起面板"));
    close->setFocusPolicy(Qt::NoFocus);
    close->setCursor(Qt::ArrowCursor);
    connect(close, &QToolButton::clicked, this, &TitleBar::closeRequested);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(icon);
    layout->addWidget(m_title);
    layout->addStretch(1);
    layout->addWidget(m_gear);
    layout->addWidget(close);

    // 用 objectName 选择器，不用类名选择器 —— QSS 的类名选择器匹配不上自定义类
    setStyleSheet(QStringLiteral(
        "QWidget#clipNoteTitleBar QToolButton { border: none; background: transparent; }"
        "QWidget#clipNoteTitleBar QToolButton:hover { background: #e3e3e3; }"
        "QWidget#clipNoteTitleBar QToolButton:pressed { background: #d4d4d4; }"
        "QWidget#clipNoteTitleBar QLabel { color: #3a3a3a; }"));
}

void TitleBar::showGearMenu()
{
    QMenu menu;
    QAction *check = menu.addAction(QStringLiteral("检查更新…"));
    menu.addSeparator();
    QAction *about = menu.addAction(QStringLiteral("关于 ClipNote"));

    // 从齿轮下方弹出。菜单比面板宽也没关系：QMenu 会自己挪回屏幕里。
    QAction *chosen = menu.exec(m_gear->mapToGlobal(QPoint(0, m_gear->height())));
    if (!chosen)
        return;

    if (chosen == check)
        emit checkUpdateRequested();
    else if (chosen == about)
        emit aboutRequested();
}

void TitleBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    // 交给系统去拖：它能正确处理多显示器、贴边吸附、以及和窗口管理器的配合，
    // 自己算 delta 再 move() 这些全丢。
    QWindow *handle = window() ? window()->windowHandle() : nullptr;
    const bool accepted = handle && handle->startSystemMove();

    if (!accepted) {
        // 本窗口是「永不激活」的，系统有可能不受理 —— 那就自己跟鼠标。
        m_dragging = true;
        m_dragOffset = event->globalPosition().toPoint() - window()->frameGeometry().topLeft();
    }

    QWidget::mousePressEvent(event);
}

void TitleBar::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        window()->move(event->globalPosition().toPoint() - m_dragOffset);
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void TitleBar::mouseReleaseEvent(QMouseEvent *event)
{
    m_dragging = false;
    QWidget::mouseReleaseEvent(event);
}

void TitleBar::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0xF3, 0xF3, 0xF3));
    painter.setPen(QColor(0xDC, 0xDC, 0xDC));
    painter.drawLine(0, height() - 1, width(), height() - 1);
}

} // namespace ClipNote
