#include "ui/ClipDelegate.h"

#include "model/ClipStore.h"

#include <QDateTime>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

namespace ClipNote {
namespace {

// 行高：留白要够，但也不能太松 —— 面板默认才 440 高，一屏放不下几条就要一直翻。
constexpr int kRowHeight = 48;

// 圆角底的左右内缩：留一点缝，方块才不贴着列表边缘，看着才像「卡片」
constexpr int kHighlightInset = 3;
constexpr int kPadding = 10;

// 左侧那条强调竖条占用的一条空槽。**不管有没有选中都留着**，
// 否则选中时正文会横向跳一下。
constexpr int kAccentGutter = 8;

// 左侧的缩略图 / 字形区。所有行都留出这块，正文才能左边对齐。
constexpr int kLeadingSize = 34;
constexpr int kLeadingGap = 11;

constexpr int kTimeGap = 10;

const QColor kTextColor(0x1F, 0x1F, 0x1F);
const QColor kTimeColor(0x9E, 0x9E, 0x9E);
const QColor kGlyphColor(0xC4, 0xC4, 0xC4);
const QColor kThumbEdge(0xE0, 0xE0, 0xE0);
const QColor kHoverColor(0xF2, 0xF5, 0xFB);
const QColor kCurrentColor(0xE2, 0xEC, 0xFD);
const QColor kAccentColor(0x2F, 0x6F, 0xED);

QString relativeTime(const QDateTime &when)
{
    if (!when.isValid())
        return {};

    const qint64 secs = when.secsTo(QDateTime::currentDateTime());

    if (secs < 60)
        return QStringLiteral("刚刚");
    if (secs < 3600)
        return QStringLiteral("%1 分钟前").arg(secs / 60);
    if (secs < 86400)
        return QStringLiteral("%1 小时前").arg(secs / 3600);
    if (secs < 7 * 86400)
        return QStringLiteral("%1 天前").arg(secs / 86400);

    return when.toString(QStringLiteral("M月d日"));
}

// 文本条目的占位字形：三条长度递减的横线，像个段落
void drawTextGlyph(QPainter *painter, const QRectF &box, const QColor &color)
{
    QPen pen(color);
    pen.setWidthF(1.6);
    pen.setCapStyle(Qt::RoundCap);
    painter->setPen(pen);

    const qreal mid = box.center().y();
    painter->drawLine(QPointF(box.left(), box.top()), QPointF(box.right(), box.top()));
    painter->drawLine(QPointF(box.left(), mid), QPointF(box.right() - 3, mid));
    painter->drawLine(QPointF(box.left(), box.bottom()), QPointF(box.right() - 6, box.bottom()));
}

void drawThumbnail(QPainter *painter, const QRectF &box, const QPixmap &thumb)
{
    if (thumb.isNull())
        return;

    QPainterPath clip;
    clip.addRoundedRect(box, 4, 4);

    painter->save();
    painter->setClipPath(clip);
    const QSizeF scaled = QSizeF(thumb.size()).scaled(box.size(), Qt::KeepAspectRatio);
    const QRectF target(box.center().x() - scaled.width() / 2,
                        box.center().y() - scaled.height() / 2,
                        scaled.width(), scaled.height());
    painter->drawPixmap(target, thumb, QRectF(thumb.rect()));
    painter->restore();

    // 描一圈很淡的边，浅色图才不会糊进白底里
    painter->setPen(kThumbEdge);
    painter->setBrush(Qt::NoBrush);
    painter->drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
}

} // namespace

QRect leadingRect(const QRect &row)
{
    const int contentLeft = row.left() + kPadding + kAccentGutter;
    return QRect(contentLeft, row.top() + (row.height() - kLeadingSize) / 2,
                 kLeadingSize, kLeadingSize);
}

ClipDelegate::ClipDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

QSize ClipDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const
{
    return QSize(option.rect.width(), kRowHeight);
}

void ClipDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                         const QModelIndex &index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    const QRect row = option.rect;
    const bool hovered = option.state & QStyle::State_MouseOver;
    const bool current = option.state & QStyle::State_Selected;

    // 选中态画得比悬停重一点：面板点完不关闭，用户要靠它认出「刚才粘的是哪条」
    if (hovered || current) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(current ? kCurrentColor : kHoverColor);
        painter->drawRoundedRect(QRectF(row).adjusted(kHighlightInset, kHighlightInset,
                                                      -kHighlightInset, -kHighlightInset),
                                 7, 7);
    }

    // 当前那条左侧一道蓝色竖条
    if (current) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(kAccentColor);
        const QRectF bar(row.left() + kHighlightInset + 2,
                         row.center().y() - (kRowHeight - 26) / 2.0,
                         3, kRowHeight - 26);
        painter->drawRoundedRect(bar, 1.5, 1.5);
    }

    const QRect leading = leadingRect(row);

    if (index.data(ClipStore::KindRole).toInt() == int(Clip::Image)) {
        drawThumbnail(painter, QRectF(leading), index.data(ClipStore::ThumbRole).value<QPixmap>());
    } else {
        drawTextGlyph(painter, QRectF(leading).adjusted(8, 9, -8, -9), kGlyphColor);
    }

    const QRect textArea(leading.right() + kLeadingGap, row.top(),
                         qMax(0, row.right() - kPadding - (leading.right() + kLeadingGap)),
                         row.height());

    // 右边：相对时间，用比正文小一号的字
    QFont timeFont = option.font;
    timeFont.setPointSizeF(qMax(7.0, option.font.pointSizeF() * 0.82));
    const QString time = relativeTime(index.data(ClipStore::CapturedAtRole).toDateTime());
    const int timeWidth = time.isEmpty() ? 0 : QFontMetrics(timeFont).horizontalAdvance(time);

    if (timeWidth > 0) {
        painter->setFont(timeFont);
        painter->setPen(kTimeColor);
        painter->drawText(QRect(textArea.right() - timeWidth, textArea.top(),
                                timeWidth, textArea.height()),
                          Qt::AlignRight | Qt::AlignVCenter, time);
    }

    // 左边：正文，超出就省略号
    painter->setFont(option.font);
    painter->setPen(kTextColor);

    int textRight = textArea.right();
    if (timeWidth > 0)
        textRight -= timeWidth + kTimeGap;
    const QRect textRect(textArea.left(), textArea.top(),
                         qMax(0, textRight - textArea.left()), textArea.height());

    const QString text = index.data(Qt::DisplayRole).toString();
    const QString elided =
        QFontMetrics(option.font).elidedText(text, Qt::ElideRight, textRect.width());
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, elided);

    painter->restore();
}

} // namespace ClipNote
