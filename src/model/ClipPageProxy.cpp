#include "model/ClipPageProxy.h"

#include "AppConfig.h"

#include <QSortFilterProxyModel>

namespace ClipNote {

ClipPageProxy::ClipPageProxy(QObject *parent) : QAbstractProxyModel(parent) {}

int ClipPageProxy::pageSize() const
{
    return itemsPerPage();
}

int ClipPageProxy::sourceRowCount() const
{
    return sourceModel() ? sourceModel()->rowCount() : 0;
}

void ClipPageProxy::setSourceModel(QAbstractItemModel *sourceModel)
{
    if (this->sourceModel())
        this->sourceModel()->disconnect(this);

    QAbstractProxyModel::setSourceModel(sourceModel);

    m_limit = pageSize();
    m_visible = qMin(sourceRowCount(), m_limit);

    if (sourceModel) {
        // **必须自己接**：QAbstractProxyModel 跟 QSortFilterProxyModel 不一样，
        // 它**不转发任何源信号**（setSourceModel 只把 rows/columnsInserted/Removed 接到
        // 几个空槽，modelReset/dataChanged/layoutChanged 根本不接）。
        // 漏了的症状是「首次 prepend 之后列表死活不动」，极难联想到是这里。
        connect(sourceModel, &QAbstractItemModel::rowsInserted, this,
                &ClipPageProxy::onSourceInserted);
        connect(sourceModel, &QAbstractItemModel::rowsRemoved, this,
                &ClipPageProxy::onSourceRemoved);
        connect(sourceModel, &QAbstractItemModel::dataChanged, this,
                &ClipPageProxy::onSourceDataChanged);
        connect(sourceModel, &QAbstractItemModel::modelReset, this,
                &ClipPageProxy::onSourceReset);
        // 下层重过滤（切选项卡、收藏变化）可能只发 layoutChanged —— 那也要重新对窗口
        connect(sourceModel, &QAbstractItemModel::layoutChanged, this,
                &ClipPageProxy::onSourceReset);
    }
}

int ClipPageProxy::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_visible;
}

int ClipPageProxy::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : 1;
}

QModelIndex ClipPageProxy::index(int row, int column, const QModelIndex &parent) const
{
    if (parent.isValid() || row < 0 || row >= m_visible || column != 0)
        return {};
    // 恒等前缀：视图第 row 行就是下层第 row 行
    return createIndex(row, column);
}

QModelIndex ClipPageProxy::parent(const QModelIndex &) const
{
    return {};   // 列表模型，没有层级
}

QModelIndex ClipPageProxy::mapToSource(const QModelIndex &proxyIndex) const
{
    if (!proxyIndex.isValid() || !sourceModel())
        return {};
    return sourceModel()->index(proxyIndex.row(), proxyIndex.column());
}

QModelIndex ClipPageProxy::mapFromSource(const QModelIndex &sourceIndex) const
{
    if (!sourceIndex.isValid() || sourceIndex.row() >= m_visible)
        return {};
    return createIndex(sourceIndex.row(), sourceIndex.column());
}

int ClipPageProxy::storeRow(const QModelIndex &proxyIndex) const
{
    if (!proxyIndex.isValid() || !sourceModel())
        return -1;

    const QModelIndex filterIndex = mapToSource(proxyIndex);
    if (!filterIndex.isValid())
        return -1;

    // 下层是过滤代理，再往下才是 ClipStore
    if (auto *filter = qobject_cast<QSortFilterProxyModel *>(sourceModel())) {
        const QModelIndex storeIndex = filter->mapToSource(filterIndex);
        return storeIndex.isValid() ? storeIndex.row() : -1;
    }
    return filterIndex.row();
}

bool ClipPageProxy::hasMore() const
{
    return m_visible < sourceRowCount();
}

void ClipPageProxy::loadMore()
{
    if (!hasMore())
        return;

    const int old = m_visible;
    m_limit += pageSize();
    const int next = qMin(sourceRowCount(), m_limit);

    // **追加在尾部**，不是重置：QListView 在 ScrollPerItem 下 value 不变、maximum 变大，
    // 所以滚动位置不会被顶回顶部 —— 这正是这个类存在的理由。
    beginInsertRows(QModelIndex(), old, next - 1);
    m_visible = next;
    endInsertRows();
}

void ClipPageProxy::resetPaging()
{
    m_limit = pageSize();
    onSourceReset();
}

void ClipPageProxy::refreshWindow()
{
    onSourceReset();
}

// 所有源变化都收敛到这一个函数。
//   shiftFrom    ：下层被改动的位置（因为映射恒等，也就是视图位置）
//   newVisible   ：改动之后该显示多少行
//   contentShift ：该位置之后，可见行的**内容**是否整体挪了位置
void ClipPageProxy::applyWindow(int shiftFrom, int newVisible, bool contentShift)
{
    const int old = m_visible;
    shiftFrom = qBound(0, shiftFrom, qMin(old, newVisible));

    if (newVisible > old) {
        // 插入的是**多出来的那几行**，插在 shiftFrom 处 —— 不是「从 shiftFrom 到末尾」。
        // 前面来了新条目时就是「在 0 处插一行，原来的整体下移一格」。
        const int count = newVisible - old;
        beginInsertRows(QModelIndex(), shiftFrom, shiftFrom + count - 1);
        m_visible = newVisible;
        endInsertRows();
    } else if (newVisible < old) {
        beginRemoveRows(QModelIndex(), newVisible, old - 1);
        m_visible = newVisible;
        endRemoveRows();
    }

    // 窗口里还留着的那部分，内容可能整体挪了位置（比如前面插进来一条、后面掉出去一条）——
    // 这时 rowCount 没变，但每一行画的东西都变了，必须让视图重画。
    // **不带 roles**：换的是另一条 Clip，不是某个字段。
    const int last = qMin(m_visible, old) - 1;
    if (contentShift && last >= shiftFrom)
        emit dataChanged(index(shiftFrom, 0), index(last, 0));
}

void ClipPageProxy::onSourceInserted(const QModelIndex &, int first, int)
{
    const int next = qMin(sourceRowCount(), m_limit);
    // 插入点落在窗口里（或在窗口上方）时，窗口内容整体下移
    applyWindow(qMin(first, m_visible), next, true);
}

void ClipPageProxy::onSourceRemoved(const QModelIndex &, int first, int)
{
    const int next = qMin(sourceRowCount(), m_limit);
    applyWindow(qMin(first, m_visible), next, true);
}

void ClipPageProxy::onSourceDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight,
                                        const QList<int> &roles)
{
    // 纯透传，roles 原样带下去（收藏那颗星就是靠这个只重画一个字段）
    const int last = qMin(bottomRight.row(), m_visible - 1);
    if (topLeft.row() <= last)
        emit dataChanged(index(topLeft.row(), 0), index(last, 0), roles);
}

void ClipPageProxy::onSourceReset()
{
    // 位置不可知（重过滤会 fork 掉下层的映射，每一行对应的内容都可能变）——
    // 整窗重读最省心，也最不容易错。
    const int next = qMin(sourceRowCount(), m_limit);
    applyWindow(0, next, false);
    if (m_visible > 0)
        emit dataChanged(index(0, 0), index(m_visible - 1, 0));
}

} // namespace ClipNote
