#pragma once

#include <QRect>
#include <QStyledItemDelegate>

namespace ClipNote {

// 一行里左侧那块「类型区」的矩形：图片是缩略图，文本是那个段落字形。
//
// 画行的时候用它，列表判断「鼠标是不是压在这一小块上」的时候也用它 —— 悬停出大图
// 预览只在缩略图上触发。两边各算一遍的话，改了留白就会对不上。
QRect leadingRect(const QRect &row);

// 一行右侧那颗收藏星星。画的位置和点的地方分开：
// 星星本身只有 16px，比 Windows 建议的最小点击目标（约 24px）小得多，
// 点偏一点就落到整行上 = 往用户当前窗口注入一次 Ctrl+V，代价比误触大得多，
// 所以命中区四周各放 4px 容差。
QRect starRect(const QRect &row);
QRect starHitRect(const QRect &row);

// 剪贴板历史每一行的画法：左边一行内容（超出省略），右边一个灰掉的相对时间，
// 鼠标悬停的行和「最近点过的那条」有圆角底色。
//
// 为什么自绘而不是写 QSS：QSS 对 QListView::item 只能改背景/边框这类很有限的东西，
// 做不出圆角底、左右分栏、以及正文和时间的字号层次。自绘一遍反而更直接。
class ClipDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit ClipDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;
};

} // namespace ClipNote
