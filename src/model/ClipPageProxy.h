#pragma once

#include <QAbstractProxyModel>

namespace ClipNote {

// 「每次只显示 20 条，滚到底再加 20 条」的分页层。
//
// 它在链条里是**最上面那层**，挂在视图上：
//
//     ClipStore（全量，rowCount 永远是全部）
//        └─ ClipFilterProxy（选项卡过滤）
//             └─ ClipPageProxy（这里，只暴露前 N 行）
//                  └─ QListView
//
// **为什么分页不塞进 ClipStore**：落盘是 `for (row = 0; row < store.rowCount(); ++row)`，
// 一旦 rowCount() 变成「当前页」，写出去的 history.json 就只剩 20 条 ——
// 不崩、不报错，重启后才发现历史大半没了。那是这次唯一会静默丢数据的做法。
//
// **为什么是"最上面那层"**：分页要作用在**过滤之后**的集合上（收藏页显示的必须是
// 前 20 个收藏，而不是"前 20 条里的收藏"）。放在过滤层上面正好。
//
// **为什么不用 `filterAcceptsRow(row){ return row < limit; }` 这个更省的写法**：
// 那样 loadMore() 只能靠 invalidateFilter()。自检量的结果是它发 **rowsRemoved**
// （见 ClipFilterProxy.h），那意味着"加载更多"会表现为先删光再插回来 ——
// 视图会重排、滚动位置会跳。而"滚到底加载更多不跳回顶部"正是这个类存在的唯一理由，
// 所以加载这条路必须是干净的 beginInsertRows，不能借道 invalidateFilter。
//
// 状态只有两个：m_limit（20/40/…）和 m_visible = min(下层行数, m_limit)，也就是 rowCount()。
// **映射是恒等前缀**（视图第 i 行就是下层第 i 行），所以不需要维护任何映射表。
class ClipPageProxy : public QAbstractProxyModel
{
    Q_OBJECT

public:
    explicit ClipPageProxy(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    QModelIndex mapToSource(const QModelIndex &proxyIndex) const override;
    QModelIndex mapFromSource(const QModelIndex &sourceIndex) const override;

    void setSourceModel(QAbstractItemModel *sourceModel) override;

    // 视图行号 → 最底下 ClipStore 的行号。
    //
    // 索引现在有三层，取数据可以直接 index.data(...)（Qt 会一层层转发），
    // 但**反查源行号不能靠 mapToSource** —— 那给出的是中间那层（过滤代理）的行号。
    // 需要删条目、设收藏、懒加载原图的地方都用这个。
    int storeRow(const QModelIndex &proxyIndex) const;

    bool hasMore() const;
    int visibleCount() const { return m_visible; }

    // 再加一页。**只发 beginInsertRows(旧数, 新数-1) 追加在尾部** ——
    // 追加后滚动条 value 不变、maximum 变大，用户"已经不在底部了"，
    // 所以既不会连锁狂加，也不会跳回顶部。
    void loadMore();

    // 切选项卡时把页数拨回一页。
    void resetPaging();

    // 下层的过滤结果变了（收藏增删、切换模式）之后调一次，重新对窗口。
    // **刻意由调用方显式调，而不是接在下层的信号上**：这样即使以后 Qt 改了
    // invalidateFilter() 发什么信号，UI 也不会错。
    void refreshWindow();

private:
    void applyWindow(int shiftFrom, int newVisible, bool contentShift);
    int sourceRowCount() const;
    int pageSize() const;

    void onSourceInserted(const QModelIndex &parent, int first, int last);
    void onSourceRemoved(const QModelIndex &parent, int first, int last);
    void onSourceDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight,
                             const QList<int> &roles);
    void onSourceReset();

    int m_limit = 0;     // 0 = 还没设过，用 pageSize()
    int m_visible = 0;
};

} // namespace ClipNote
