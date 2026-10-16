#include "model/ClipFilterProxy.h"

#include "model/ClipStore.h"

namespace ClipNote {

ClipFilterProxy::ClipFilterProxy(QObject *parent) : QSortFilterProxyModel(parent) {}

void ClipFilterProxy::setMode(Mode mode)
{
    if (m_mode == mode)
        return;

    m_mode = mode;
    invalidateFilter();
}

bool ClipFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    if (m_mode == History)
        return true;

    const QModelIndex source = sourceModel()->index(sourceRow, 0, sourceParent);
    return source.data(ClipStore::FavoriteRole).toBool();
}

} // namespace ClipNote
