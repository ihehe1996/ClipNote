#include "update/UpdateChecker.h"

#include "AppConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace ClipNote {
namespace {

// 一步到位构造请求，省得 check 和 download 两处各写一遍头。
//
// User-Agent 是**必须**的：GitHub 的 API 对没有 UA 的请求直接回 403，
// 报的还是「rate limit exceeded」这种会把人带偏的消息。
QNetworkRequest makeRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("ClipNote/%1")
                          .arg(QCoreApplication::applicationVersion()));
    request.setRawHeader("Accept", "application/vnd.github+json");

    // 别让一次检查卡住界面：15 秒还没动静就当超时。
    // 对下载来说这个值是「两个数据块之间」的超时，不是总时长，所以不碍事。
    request.setTransferTimeout(15000);

    // 附件地址会 302 到 objects.githubusercontent.com。
    // NoLessSafeRedirectPolicy 是 Qt6 的默认值，这里写出来是为了让它显式可见 ——
    // 换掉它就意味着允许 https 降级到 http，那不行。
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    return request;
}

// 把 HTTP 状态码翻译成能给用户看的一句话
QString describeHttpError(int status, const QString &reason)
{
    switch (status) {
    case 0:
        return QStringLiteral("连不上 GitHub");
    case 403:
        // GitHub 对未认证的 API 调用是每分钟 60 次
        return QStringLiteral("GitHub 拒绝了这次请求（403），多半是短时间内查得太频，等几分钟再试");
    case 404:
        return QStringLiteral("找不到这个仓库或它还没有发布过 Release（404）");
    default:
        return QStringLiteral("GitHub 返回 HTTP %1 %2").arg(status).arg(reason);
    }
}

} // namespace

UpdateChecker::UpdateChecker(QObject *parent) : QObject(parent) {}

UpdateChecker::~UpdateChecker()
{
    // m_sink 是裸的 new（它要跨 reply 的生命周期活），这里收尾
    teardownDownload();
}

bool UpdateChecker::isBusy() const
{
    return !m_reply.isNull();
}

void UpdateChecker::abort()
{
    if (m_reply) {
        // 先断开再 abort，否则 abort 触发的 finished 会把半截下载当成失败报上去
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    teardownDownload();
}

void UpdateChecker::check()
{
    const QString repo = updateRepo();
    if (repo.isEmpty() || !repo.contains(QLatin1Char('/'))) {
        Result result;
        result.error = QStringLiteral("还没有配置更新源。填好 src/AppConfig.h 里的 updateRepo() "
                                      "（形如 用户名/仓库名）之后才能检查更新。");
        emit checkFinished(result);
        return;
    }

    abort();

    const QUrl url(QStringLiteral("https://api.github.com/repos/%1/releases/latest")
                       .arg(repo.trimmed()));
    m_reply = m_network.get(makeRequest(url));
    connect(m_reply, &QNetworkReply::finished, this, &UpdateChecker::onCheckReplyFinished);
}

void UpdateChecker::onCheckReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply)
        return;

    reply->deleteLater();
    if (m_reply == reply)
        m_reply = nullptr;

    Result result;
    result.current = Version{};   // 下面解析；解析不出来就保持 0.0.0

    const QVariant statusAttribute = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const int status = statusAttribute.toInt();

    // 网络层直接失败（DNS、TLS、超时）。HttpStatusCode 拿不到才走这里 ——
    // 服务端返回了 4xx/5xx 的报文时 status 是有值的，那种走下面的分支。
    if (reply->error() != QNetworkReply::NoError && status == 0) {
        result.error = QStringLiteral("%1（%2）").arg(describeHttpError(0, QString()),
                                                      reply->errorString());
        emit checkFinished(result);
        return;
    }

    if (status != 200) {
        result.error = describeHttpError(status, reply->attribute(
                                                    QNetworkRequest::HttpReasonPhraseAttribute)
                                                    .toString());
        emit checkFinished(result);
        return;
    }

    ReleaseInfo info;
    QString parseError;
    if (!parseGitHubRelease(reply->readAll(), &info, &parseError)) {
        result.error = QStringLiteral("解析 GitHub 的响应失败：%1").arg(parseError);
        emit checkFinished(result);
        return;
    }

    // 本程序的版本解析不出来就没法比 —— 这属于「配置写错了」，
    // 不能当成「没有更新」静默吞掉，否则永远查不出问题。
    if (!parseVersion(QCoreApplication::applicationVersion(), &result.current)) {
        result.error = QStringLiteral("本程序的版本号「%1」不是合法版本号，没法比较")
                           .arg(QCoreApplication::applicationVersion());
        emit checkFinished(result);
        return;
    }

    Version latest;
    if (!parseVersion(info.tagName, &latest)) {
        result.error = QStringLiteral("Release 的 tag「%1」不是合法版本号").arg(info.tagName);
        emit checkFinished(result);
        return;
    }

    result.ok = true;
    result.release = info;
    result.latest = latest;
    result.updateAvailable = isNewerVersion(latest, result.current);

    emit checkFinished(result);
}

void UpdateChecker::download(const ReleaseAsset &asset, const QString &destPath)
{
    abort();

    // 目标目录可能还不存在（第一次下载时临时目录是新建的）
    const QFileInfo info(destPath);
    if (!QDir().mkpath(info.absolutePath())) {
        emit downloadFailed(QStringLiteral("建不出目录 %1").arg(info.absolutePath()));
        return;
    }

    m_sink = new QFile(destPath);
    if (!m_sink->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString reason = m_sink->errorString();
        teardownDownload();
        emit downloadFailed(QStringLiteral("打不开 %1：%2").arg(destPath, reason));
        return;
    }

    m_destPath = destPath;
    m_asset = asset;
    m_hash.reset();
    m_written = 0;

    m_reply = m_network.get(makeRequest(QUrl(asset.downloadUrl)));
    connect(m_reply, &QNetworkReply::readyRead, this, &UpdateChecker::onDownloadReadyRead);
    connect(m_reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) { emit downloadProgress(received, total); });
    connect(m_reply, &QNetworkReply::finished, this, &UpdateChecker::onDownloadReplyFinished);
}

void UpdateChecker::onDownloadReadyRead()
{
    if (!m_reply || !m_sink)
        return;

    const QByteArray chunk = m_reply->readAll();
    if (chunk.isEmpty())
        return;

    m_hash.addData(chunk);
    if (m_sink->write(chunk) != chunk.size()) {
        const QString reason = m_sink->errorString();
        abort();
        emit downloadFailed(QStringLiteral("写盘失败：%1").arg(reason));
        return;
    }
    m_written += chunk.size();
}

void UpdateChecker::onDownloadReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply)
        return;

    reply->deleteLater();
    if (m_reply == reply)
        m_reply = nullptr;

    // 收尾：把可能还留在缓冲区里的最后一块取出来
    onDownloadReadyRead();

    const QString destPath = m_destPath;
    const ReleaseAsset asset = m_asset;
    const qint64 written = m_written;
    const QString actualHash = QString::fromLatin1(m_hash.result().toHex());

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError error = reply->error();

    // 关文件这一步不能省：不关的话下面 statSize 拿到的可能还是旧的长度
    teardownDownload();

    if (error != QNetworkReply::NoError && status == 0) {
        QFile::remove(destPath);
        emit downloadFailed(QStringLiteral("下载中断：%1").arg(reply->errorString()));
        return;
    }
    if (status != 200 && status != 0) {
        QFile::remove(destPath);
        emit downloadFailed(describeHttpError(status, QString()));
        return;
    }

    // ---- 完整性校验。这一段是自更新的关键防线： ----
    // 一个被截断或者被中间人换掉的 exe 要是替了上去，用户手上就是个坏掉的程序，
    // 而且很可能连「检查更新」都再点不动了（因为新的 exe 起不来）。
    if (asset.size >= 0 && written != asset.size) {
        QFile::remove(destPath);
        emit downloadFailed(QStringLiteral("下载的文件大小不对（预期 %1 字节，实收 %2），已丢弃")
                                .arg(asset.size)
                                .arg(written));
        return;
    }
    if (!asset.sha256.isEmpty() && actualHash.compare(asset.sha256, Qt::CaseInsensitive) != 0) {
        QFile::remove(destPath);
        emit downloadFailed(QStringLiteral("下载的文件校验不过（sha256 对不上），已丢弃。\n"
                                           "可能是网络出了问题，也可能是这个 Release 的附件"
                                           "和它登记的摘要不一致。"));
        return;
    }

    emit downloadFinished(destPath);
}

void UpdateChecker::teardownDownload()
{
    if (m_sink) {
        m_sink->close();
        delete m_sink;
        m_sink = nullptr;
    }
    m_destPath.clear();
    m_asset = ReleaseAsset{};
    m_written = 0;
}

} // namespace ClipNote
