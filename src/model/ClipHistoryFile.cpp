#include "model/ClipHistoryFile.h"

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
    for (int row = 0; row < store.rowCount(); ++row) {
        const QModelIndex index = store.index(row, 0);
        // 图片不落盘（见头文件说明），跳过
        if (index.data(ClipStore::KindRole).toInt() != int(Clip::Text))
            continue;

        QJsonObject item;
        item.insert(kTextKey, index.data(ClipStore::TextRole).toString());
        item.insert(kAtKey, index.data(ClipStore::CapturedAtRole).toDateTime().toString(Qt::ISODate));
        item.insert(kFavoriteKey, index.data(ClipStore::FavoriteRole).toBool());
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

    QVector<Clip> items;
    const QJsonArray array = root.value(QStringLiteral("items")).toArray();
    items.reserve(array.size());
    for (const QJsonValue &value : array) {
        const QJsonObject item = value.toObject();
        const QString text = item.value(kTextKey).toString();
        if (text.isEmpty())
            continue;

        Clip clip;
        clip.kind = Clip::Text;
        clip.text = text;
        clip.favorite = item.value(kFavoriteKey).toBool();
        clip.capturedAt = QDateTime::fromString(item.value(kAtKey).toString(), Qt::ISODate);
        items.append(clip);
    }

    store->restoreTexts(items);
    return true;
}

} // namespace ClipNote
