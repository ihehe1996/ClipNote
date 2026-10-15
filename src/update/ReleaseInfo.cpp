#include "update/ReleaseInfo.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace ClipNote {
namespace {

ReleaseAsset assetFromJson(const QJsonObject &object)
{
    ReleaseAsset asset;
    asset.name = object.value(QStringLiteral("name")).toString();
    asset.downloadUrl = object.value(QStringLiteral("browser_download_url")).toString();
    asset.size = static_cast<qint64>(object.value(QStringLiteral("size")).toDouble(-1));

    // "sha256:abc..." —— 这个字段是 GitHub 后加的，老的 release 里没有
    const QString digest = object.value(QStringLiteral("digest")).toString();
    if (digest.startsWith(QStringLiteral("sha256:"), Qt::CaseInsensitive))
        asset.sha256 = digest.mid(7).trimmed().toLower();

    return asset;
}

} // namespace

bool parseGitHubRelease(const QByteArray &json, ReleaseInfo *out, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return fail(QStringLiteral("响应不是合法 JSON：%1").arg(parseError.errorString()));
    if (!document.isObject())
        return fail(QStringLiteral("响应的顶层不是 JSON 对象"));

    const QJsonObject root = document.object();

    // GitHub 在仓库不存在或没有 release 时也回 200/404 带 message 字段的对象，
    // 所以这里要显式挡一下，否则会解析出一个「版本号是空串」的假 release
    if (root.contains(QStringLiteral("message")) && !root.contains(QStringLiteral("tag_name")))
        return fail(QStringLiteral("GitHub 返回：%1")
                        .arg(root.value(QStringLiteral("message")).toString()));

    ReleaseInfo info;
    info.tagName = root.value(QStringLiteral("tag_name")).toString();
    info.title = root.value(QStringLiteral("name")).toString();
    info.notes = root.value(QStringLiteral("body")).toString();
    info.pageUrl = root.value(QStringLiteral("html_url")).toString();

    // 时间是 ISO8601 带 Z 的，Qt::ISODate 认得
    info.publishedAt = QDateTime::fromString(
        root.value(QStringLiteral("published_at")).toString(), Qt::ISODate);

    const QJsonArray assets = root.value(QStringLiteral("assets")).toArray();
    info.assets.reserve(assets.size());
    for (const QJsonValue &value : assets) {
        if (value.isObject())
            info.assets.append(assetFromJson(value.toObject()));
    }

    if (info.tagName.trimmed().isEmpty())
        return fail(QStringLiteral("响应里没有 tag_name，拿不到版本号"));

    if (out)
        *out = info;
    return true;
}

const ReleaseAsset *pickUpdateAsset(const ReleaseInfo &info)
{
    // 文件名看着像安装程序的，一律不要。
    //
    // 这条是必须的，不是保险：自从 Release 里除了便携 zip 还挂上安装程序，
    // 安装程序也是 .exe，名字里又没有主程序那个名字，于是 findStagedExe() 的
    // 「只有一个 .exe 就用它」兜底会把它选中 —— 结果是把用户正在用的
    // ClipNote.exe **替换成安装程序本身**，双击弹出个安装界面，程序等于没了。
    const auto looksLikeInstaller = [](const QString &lowerName) {
        return lowerName.contains(QStringLiteral("setup"))
               || lowerName.contains(QStringLiteral("install"));
    };

    const ReleaseAsset *zip = nullptr;
    const ReleaseAsset *exe = nullptr;

    for (const ReleaseAsset &asset : info.assets) {
        // 附件可能没有下载地址（rare，但 JSON 里允许），那种不能选
        if (asset.downloadUrl.isEmpty())
            continue;

        const QString lower = asset.name.toLower();
        if (looksLikeInstaller(lower))
            continue;

        if (!zip && lower.endsWith(QStringLiteral(".zip")))
            zip = &asset;
        else if (!exe && lower.endsWith(QStringLiteral(".exe")))
            exe = &asset;
    }

    return zip ? zip : exe;
}

} // namespace ClipNote
