#include "model/ClipStore.h"

#include "model/ClipImageFile.h"

#include <QHashFunctions>

namespace ClipNote {
namespace {

// 缩略图的最长边。列表里显示得比这还小，留点余量给高分屏。
constexpr int kThumbSize = 96;

// 列表里一条只占一行：剪贴板内容可能是一整篇文章，换行原样塞进去会把行高撑爆。
// 原文不受影响，完整内容通过 TextRole / ToolTipRole 取。
QString oneLine(const QString &text)
{
    QString summary = text;
    summary.replace(QLatin1Char('\n'), QLatin1Char(' '));
    summary.replace(QLatin1Char('\r'), QLatin1Char(' '));
    summary.replace(QLatin1Char('\t'), QLatin1Char(' '));
    return summary.simplified();
}

QPixmap makeThumb(const QImage &image)
{
    return QPixmap::fromImage(image.scaled(kThumbSize, kThumbSize,
                                           Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

} // namespace

uint ClipStore::imageHash(const QImage &image)
{
    if (image.isNull())
        return 0;

    // 按**缩略图**算，不按原图：原图几十 MB，每粘贴一次都哈希一遍会卡住界面。
    // 缩略图最长边 96px，哈希开销可以忽略。两张不同的图缩到 96px 完全一样的概率
    // 极低，真撞上了也只是少存一条，无伤大雅。
    const QPixmap thumb = makeThumb(image);
    const QImage thumbImage = thumb.toImage().convertToFormat(QImage::Format_RGBA8888);

    return qHashBits(thumbImage.constBits(), size_t(thumbImage.sizeInBytes()),
                     uint(image.width() * 31 + image.height()));
}

ClipStore::ClipStore(QObject *parent) : QAbstractListModel(parent) {}

int ClipStore::rowCount(const QModelIndex &parent) const
{
    // 列表模型没有层级，问子项一律 0
    return parent.isValid() ? 0 : m_items.size();
}

QVariant ClipStore::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return {};

    const Clip &clip = m_items.at(index.row());

    switch (role) {
    case Qt::DisplayRole:
        // 尺寸读 imageSize 而不是 clip.image：从磁盘恢复的条目原图是空的（懒加载），
        // 问 clip.image 会得到 0 × 0
        return clip.kind == Clip::Image
                   ? QStringLiteral("图片  %1 × %2").arg(clip.imageSize.width())
                         .arg(clip.imageSize.height())
                   : oneLine(clip.text);
    // 「鼠标停住时显示什么」。文本给全文，由 MainWindow 交给 QToolTip 弹**原生气泡**
    // —— 也只有这一处会给，两边都给就会弹两个（试过，同一段文字出来两次）。
    //
    // 图片返回空串是故意的：图片的悬停提示是那个自绘的「大图预览」
    //（原生气泡塞不进位图），这里再给一句文字提示就会两个气泡打架。
    case Qt::ToolTipRole:
        return clip.kind == Clip::Image ? QString() : clip.text;
    case TextRole:
        return clip.text;
    case CapturedAtRole:
        return clip.capturedAt;
    case KindRole:
        return int(clip.kind);
    case ThumbRole:
        return clip.thumb;
    case ImageRole:
        return clip.image;
    case FavoriteRole:
        return clip.favorite;
    default:
        return {};
    }
}

int ClipStore::indexOfText(const QString &text) const
{
    for (int row = 0; row < m_items.size(); ++row) {
        if (m_items.at(row).kind == Clip::Text && m_items.at(row).text == text)
            return row;
    }
    return -1;
}

int ClipStore::indexOfImage(uint hash) const
{
    for (int row = 0; row < m_items.size(); ++row) {
        if (m_items.at(row).kind == Clip::Image && m_items.at(row).imageHash == hash)
            return row;
    }
    return -1;
}

void ClipStore::pushText(const QString &text)
{
    if (text.trimmed().isEmpty())
        return;

    const int row = indexOfText(text);

    // 已经在最前面就当没发生过，**一个信号都不发**。
    // 程序重新发布同样的剪贴板内容（窗口获得焦点时很常见）会走到这里，
    // 要是每次都不管不顾地挪一下，用户正在翻列表时选中项会被无谓地重置。
    if (row == 0)
        return;

    Clip clip;
    clip.kind = Clip::Text;
    clip.text = text;
    // 已经在历史里就沿用它的收藏标记。不抄的话「收藏过的内容再复制一次」
    // 会静悄悄地丢掉收藏 —— 因为这里是新建了一个 Clip，favorite 默认是 false。
    if (row > 0)
        clip.favorite = m_items.at(row).favorite;
    insertOrPromote(clip, row);
}

void ClipStore::pushImage(const QImage &image)
{
    if (image.isNull())
        return;

    if (qint64(image.width()) * image.height() > kMaxImagePixels)
        return; // 太大，历史全在内存里，不收

    const uint hash = imageHash(image);
    const int row = indexOfImage(hash);
    if (row == 0)
        return;

    Clip clip;
    clip.kind = Clip::Image;
    clip.imageSize = image.size();
    clip.thumb = makeThumb(image);
    clip.imageHash = hash;

    // 落盘。**立刻同步写，不走那个 1 秒防抖** —— 防抖是为「连续复制」设计的，
    // 而图片这条要是等 1 秒再写、用户 0.3 秒后退出就白复制了。
    // 代价是 PNG 编码占住 UI（典型截图几十到几百毫秒），低频操作，可以接受。
    const bool persisted = !m_imagesDir.isEmpty()
                           && saveImage(m_imagesDir, hash, image, clip.thumb);

    // 写下去了就只留缩略图（原图在磁盘上，要的时候再读）；
    // 写不下去才把原图留在内存里 —— 本次会话至少还能粘贴，重启后没了，
    // 和「不落盘」时的行为一致
    if (!persisted)
        clip.image = image;
    // 同 pushText：重复的图再复制一次不能把收藏弄丢
    if (row > 0)
        clip.favorite = m_items.at(row).favorite;
    insertOrPromote(clip, row);
}

void ClipStore::insertOrPromote(const Clip &clip, int existingRow)
{
    Clip fresh = clip;
    fresh.capturedAt = QDateTime::currentDateTime();

    if (existingRow > 0) {
        // 已经在历史里：用「删掉 + 插到头部」，而不是 beginMoveRows ——
        // QListView 对 rowsMoved 的处理历史上有坑，而这是低频操作，
        // 多发一个信号完全无所谓。
        beginRemoveRows(QModelIndex(), existingRow, existingRow);
        m_items.remove(existingRow);
        endRemoveRows();
    }

    beginInsertRows(QModelIndex(), 0, 0);
    m_items.prepend(fresh);
    endInsertRows();

    trimToLimit();
}

void ClipStore::trimToLimit()
{
    // 上限管的是**非收藏**条目的条数，收藏项永远不参与截断 ——
    // 所以这里不能像以前那样 resize() 一刀砍尾巴：要丢的那些行不连续了。
    // 从尾部往前数着删，正常情况下每次只溢出 1 条，循环一轮就结束。
    int plain = 0;
    for (const Clip &clip : m_items) {
        if (!clip.favorite)
            ++plain;
    }
    if (plain <= kMaxItems)
        return;

    for (int row = m_items.size() - 1; row >= 0 && plain > kMaxItems; --row) {
        if (m_items.at(row).favorite)
            continue;
        dropImageFile(m_items.at(row));   // 图片文件跟着条目走，不删就是永远占着磁盘
        beginRemoveRows(QModelIndex(), row, row);
        m_items.remove(row);
        endRemoveRows();
        --plain;
    }
}

void ClipStore::removeAt(int row)
{
    if (row < 0 || row >= m_items.size())
        return;

    dropImageFile(m_items.at(row));

    // 缓存那行要是被删了或者挪位了，就作废（不然 loadFullImage 会返回别人的图）
    m_cachedRow = -1;

    beginRemoveRows(QModelIndex(), row, row);
    m_items.remove(row);
    endRemoveRows();
}

void ClipStore::dropImageFile(const Clip &clip)
{
    if (clip.kind != Clip::Image || clip.imageHash == 0)
        return;
    removeImage(m_imagesDir, clip.imageHash);
}

QSet<uint> ClipStore::liveImageHashes() const
{
    QSet<uint> hashes;
    for (const Clip &clip : m_items) {
        if (clip.kind == Clip::Image && clip.imageHash != 0)
            hashes.insert(clip.imageHash);
    }
    return hashes;
}

QImage ClipStore::loadFullImage(int row)
{
    if (row < 0 || row >= m_items.size())
        return {};

    const Clip &clip = m_items.at(row);
    if (clip.kind != Clip::Image)
        return {};

    // 本次会话里复制进来的（或者没配图片目录时的）条目，原图本来就在内存里
    if (!clip.image.isNull())
        return clip.image;

    if (m_cachedRow == row && !m_cachedImage.isNull())
        return m_cachedImage;

    const QImage loaded = loadImage(m_imagesDir, clip.imageHash);
    if (loaded.isNull())
        return {};   // 文件被删了/坏了 —— 调用方按「这条失效」处理

    m_cachedRow = row;
    m_cachedImage = loaded;
    return loaded;
}

void ClipStore::clearHistory()
{
    // 从后往前删非收藏的，收藏的跳过 —— 「清空历史（保留收藏）」就是它。
    // 一条都没删（比如全是收藏）时自然一个信号也不发。
    for (int row = m_items.size() - 1; row >= 0; --row) {
        if (m_items.at(row).favorite)
            continue;
        dropImageFile(m_items.at(row));
        beginRemoveRows(QModelIndex(), row, row);
        m_items.remove(row);
        endRemoveRows();
    }
}

void ClipStore::clearFavorites()
{
    bool changedAny = false;
    for (Clip &clip : m_items) {
        if (clip.favorite) {
            clip.favorite = false;
            changedAny = true;
        }
    }
    if (!changedAny)
        return;

    // 改的是散落在各处的若干行，dataChanged 只能给一个连续区间；
    // 整表 reset 最省事，反正这是个低频操作，代价只是列表重建一次。
    beginResetModel();
    endResetModel();
}

void ClipStore::restoreTexts(const QVector<Clip> &items)
{
    beginResetModel();
    m_items = items;
    endResetModel();
}

bool ClipStore::isFavorite(int row) const
{
    return row >= 0 && row < m_items.size() && m_items.at(row).favorite;
}

void ClipStore::setFavorite(int row, bool favorite)
{
    if (row < 0 || row >= m_items.size())
        return;
    if (m_items.at(row).favorite == favorite)
        return;   // 没变就不发信号

    m_items[row].favorite = favorite;

    // 带上 roles：视图靠它知道只用重画那颗星，不必整行重画
    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx, {FavoriteRole});
}

QString ClipStore::textAt(int row) const
{
    if (row < 0 || row >= m_items.size())
        return {};
    return m_items.at(row).text;
}

} // namespace ClipNote
