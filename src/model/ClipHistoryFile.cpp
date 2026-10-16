#include "model/ClipHistoryFile.h"

#include "model/ClipImageFile.h"
#include "model/ClipStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace ClipNote {

namespace {

// 文件格式版本。以后格式变了，就在这里加分支；读到不认识的版本一律当空历史。
constexpr int kFormatVersion = 1;

const QString kTextKey = QStringLiteral("text");
const QString kAtKey = QStringLiteral("at");
const QString kFavoriteKey = QStringLiteral("favorite");
// 图片条目用的字段
const QString kKindKey = QStringLiteral("kind");
const QString kHashKey = QStringLiteral("hash");
const QString kWidthKey = QStringLiteral("w");
const QString kHeightKey = QStringLiteral("h");

const QString kKindImage = QStringLiteral("image");

} // namespace

QString historyFilePath()
{
    // AppDataLocation 在 Windows 上是 %APPDATA%\<组织>\<应用>，两者都叫 ClipNote，
    // 所以路径里会出现两次 —— 这是 QStandardPaths 的标准行为，不用手动去重。
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty())
        return {};
    return QDir(dir).filePath(QStringLiteral("history.json"));
}

bool saveHistory(const QString &path, const ClipStore &store, QString *error)
{
    if (path.isEmpty()) {
        if (error)
            *error = QStringLiteral("历史文件路径是空的");
        return false;
    }

    QJsonArray items;
    // 存的是 store 里的**全部条目**（不是"当前显示的"）—— 分页只活在代理层，
    // ClipStore 这边永远是全量。这也是「分页绝不能塞进 ClipStore」的原因：
    // 一旦 rowCount() 变成"当前页"，这里就会静默地只写出 20 条。
    for (const Clip &clip : store.items()) {
        QJsonObject item;
        item.insert(kAtKey, clip.capturedAt.toString(Qt::ISODate));
        item.insert(kFavoriteKey, clip.favorite);

        if (clip.kind == Clip::Image) {
            // 图片只记元数据，像素在 images/<hash>.png 里。尺寸要单独记：
            // 懒加载之后手里没有 QImage 可以问，而列表上「图片 1920 × 1080」要用。
            if (clip.imageHash == 0)
                continue;   // 没哈希就没法定位文件，这条存了也读不回来
            item.insert(kKindKey, kKindImage);
            item.insert(kHashKey, QString::number(clip.imageHash, 16));
            item.insert(kWidthKey, clip.imageSize.width());
            item.insert(kHeightKey, clip.imageSize.height());
        } else {
            item.insert(kTextKey, clip.text);
        }
        items.append(item);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), kFormatVersion);
    root.insert(QStringLiteral("items"), items);

    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error)
            *error = QStringLiteral("建不出目录：%1").arg(QFileInfo(path).absolutePath());
        return false;
    }

    // QSaveFile：先写同目录下的临时文件，commit() 时才改名覆盖。
    // 直接 Truncate 打开的话，写到一半崩溃/断电会把旧文件一起毁掉 ——
    // 历史是唯一一份数据，没有第二处可恢复，值得用这个方法。
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = file.errorString();
        return false;
    }

    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

bool loadHistory(const QString &path, ClipStore *store, QString *error)
{
    if (!store)
        return false;

    QFile file(path);
    if (!file.exists()) {
        // 第一次跑，文件还没有 —— 这不是错误
        if (error)
            *error = QStringLiteral("还没有历史文件");
        return false;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = QStringLiteral("历史文件解析失败：%1").arg(parseError.errorString());
        return false;
    }

    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("version")).toInt() != kFormatVersion) {
        if (error)
            *error = QStringLiteral("历史文件版本不认识");
        return false;
    }

    const QString imagesDir = store->imagesDir();

    QVector<Clip> items;
    const QJsonArray array = root.value(QStringLiteral("items")).toArray();
    items.reserve(array.size());
    for (const QJsonValue &value : array) {
        const QJsonObject item = value.toObject();

        Clip clip;
        clip.favorite = item.value(kFavoriteKey).toBool();
        clip.capturedAt = QDateTime::fromString(item.value(kAtKey).toString(), Qt::ISODate);

        if (item.value(kKindKey).toString() == kKindImage) {
            // 图片：**只读缩略图**（几 KB），原图留到要看大图/要粘贴时再读 —— 这就是懒加载。
            // 缩略图读不到说明文件没了（被删了、或者上次「写完图片、还没写 JSON」就崩了），
            // **这条直接跳过不进历史**：宁可少一条，也不要留一条点开是空白的坏条目。
            bool ok = false;
            const uint hash = item.value(kHashKey).toString().toUInt(&ok, 16);
            if (!ok || hash == 0)
                continue;

            const QPixmap thumb = loadThumb(imagesDir, hash);
            if (thumb.isNull())
                continue;

            clip.kind = Clip::Image;
            clip.imageHash = hash;
            clip.thumb = thumb;
            clip.imageSize = QSize(item.value(kWidthKey).toInt(), item.value(kHeightKey).toInt());
            // clip.image 刻意留空 —— 懒加载
            items.append(clip);
            continue;
        }

        const QString text = item.value(kTextKey).toString();
        if (text.isEmpty())
            continue;
        clip.kind = Clip::Text;
        clip.text = text;
        items.append(clip);
    }

    store->restoreTexts(items);
    return true;
}

} // namespace ClipNote
