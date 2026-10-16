#pragma once

#include <QSortFilterProxyModel>

namespace ClipNote {

// 面板顶部两个选项卡：「历史记录」看全部，「收藏」只看收藏过的。
//
// 用代理而不是让 ClipStore 自己过滤：源模型一过滤，它每个方法都得在
// 「视图行号」和「真实下标」之间换算，插一条还要判断它在新视图里排第几 —— 太容易错。
// 代理把这件事收在 mapToSource() 一处。
//
// **一个必须记住的前提**：QSortFilterProxyModel 不支持源模型的 beginMoveRows，
// 而 ClipStore::insertOrPromote() 刻意用「删掉 + 插到头部」代替 move（那边注释写了原因）。
// 换言之这个代理能成立，靠的是那边没有用 beginMoveRows —— 别去「优化」它。
class ClipFilterProxy : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    enum Mode { History, Favorites };

    explicit ClipFilterProxy(QObject *parent = nullptr);

    Mode mode() const { return m_mode; }

    // 切页。改完记得让调用方自己再刷一遍空状态。
    //
    // `invalidateFilter()` 实际发的是 **rowsRemoved**（自检里那条「信号审计」量出来的：
    // 3 行切到只剩 1 行，发两次 rowsRemoved），不是 layoutChanged、也不是 modelReset。
    // 这个结论是量出来的、不是猜的，所以上下两层的衔接可以照它写。
    void setMode(Mode mode);

    // 重新过滤。**收藏状态变了之后必须调**：代理不会自己重评估某一行的自定义
    // filterAcceptsRow，在收藏页取消收藏那一行就会赖着不走。
    //
    // invalidateFilter() 在 QSortFilterProxyModel 里是 protected，外面够不着，
    // 所以在这里开个口子。也刻意不接到源模型的 dataChanged 上 —— 那是在源信号
    // 发出的过程中重入，交给调用方在动作做完之后显式刷更安全。
    void refresh() { invalidateFilter(); }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    Mode m_mode = History;
};

} // namespace ClipNote
