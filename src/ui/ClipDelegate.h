#pragma once

#include <QStyledItemDelegate>

namespace ClipNote {

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
