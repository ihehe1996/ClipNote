#include "ui/PreviewPopup.h"

#include <QCursor>
#include <QGuiApplication>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QScreen>
#include <QVBoxLayout>

namespace ClipNote {

namespace {

// 大图预览的边长上限。再大也没意义 —— 只是想看清楚是什么图，不是拿它当看图器。
// 真正给多大还要跟屏幕比（见 availableForPreview），小屏上不能弹出一个比屏幕还大的框。
constexpr int kMaxImageSide = 640;

constexpr int kMinPreviewSide = 200;
constexpr int kPadding = 8;
constexpr int kGapToPanel = 8;

// 预览框最大能占多大：屏幕可用区域再留一圈边，免得贴边。
QSize availableForPreview(QScreen *screen)
{
    const QRect area = screen->availableGeometry();
    return QSize(qMax(kMinPreviewSide, area.width() - 4 * kGapToPanel),
                 qMax(kMinPreviewSide, area.height() - 4 * kGapToPanel));
}

} // namespace

PreviewPopup::PreviewPopup(QWidget *parent)
    : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint)
{
    // 和面板一样：显示时不激活。预览只是给人看的，抢走焦点会把用户原来那个程序的
    // 光标顶掉，「单击即粘贴」的前提就没了。
    setAttribute(Qt::WA_ShowWithoutActivating);

    // 方块边、1px 描边，和面板那圈描边一个颜色 —— 不做圆角：这个窗口没有透明底，
    // 圆角会把底下的东西（或者黑边）从四个角露出来。
    setStyleSheet(QStringLiteral("QWidget { background: #ffffff; border: 1px solid #cfcfcf; }"));

    m_content = new QLabel(this);
    m_content->setStyleSheet(QStringLiteral("border: none;"));
    m_content->setTextInteractionFlags(Qt::NoTextInteraction);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(kPadding, kPadding, kPadding, kPadding);
    layout->addWidget(m_content);
}

void PreviewPopup::showImage(const QImage &image)
{
    QPixmap pixmap = QPixmap::fromImage(image);
    if (pixmap.isNull()) {
        hide();
        return;
    }

    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QSize available = screen ? availableForPreview(screen) : QSize(kMaxImageSide, kMaxImageSide);
    const QSize cap(qMin(kMaxImageSide, available.width()),
                    qMin(kMaxImageSide, available.height()));

    // 只缩不放：原图比这个上限小的时候就按原尺寸显示，放大会糊。
    if (pixmap.width() > cap.width() || pixmap.height() > cap.height())
        pixmap = pixmap.scaled(cap, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    m_content->setPixmap(pixmap);
    adjustSize();
}

void PreviewPopup::showNextTo(const QWidget *panel, const QPoint &cursor)
{
    adjustSize();   // 内容定了才知道自己多大

    const QRect panelRect(panel->mapToGlobal(QPoint(0, 0)), panel->size());
    QScreen *screen = QGuiApplication::screenAt(cursor);
    if (!screen)
        screen = QGuiApplication::screenAt(panelRect.center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen) {
        show();
        return;
    }
    const QRect area = screen->availableGeometry();

    // 摆在面板外侧，优先右边；右边放不下就翻到左边
    int x = panelRect.right() + kGapToPanel;
    if (x + width() > area.right())
        x = panelRect.left() - kGapToPanel - width();

    // 纵向对着光标，但偏上一点 —— 光标压着的就是这一行，预览和它对齐更直观
    int y = cursor.y() - height() / 3;

    x = qBound(area.left(), x, qMax(area.left(), area.right() - width()));
    y = qBound(area.top(), y, qMax(area.top(), area.bottom() - height()));

    move(x, y);
    show();
    raise();   // 面板是永远置顶的，预览得比它更靠前
}

} // namespace ClipNote
