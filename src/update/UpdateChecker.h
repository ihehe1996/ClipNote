#pragma once

#include "update/ReleaseInfo.h"
#include "update/Version.h"

#include <QCryptographicHash>
#include <QFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>

QT_BEGIN_NAMESPACE
class QNetworkReply;
QT_END_NAMESPACE

namespace ClipNote {

// 404 的两种可能，对应的处理办法完全不一样：
// 一种是「配置写错了，去改 updateRepo()」，一种是「还没发 Release，去发一个」。
// 所以分成两条消息，而且自检里钉了一条「这两条必须能区分开」。
// 纯函数，拿出来就是为了能在自检里验。
QString repoNotFoundMessage(const QString &repo);
QString noReleaseYetMessage(const QString &repo);

// 查 GitHub Releases + 下载附件。**没有界面**，结果全走信号，
// 这样同一个类既能给「齿轮菜单里手动查」用，也能给「启动时静默查」用。
class UpdateChecker : public QObject
{
    Q_OBJECT

public:
    struct Result
    {
        bool ok = false;              // 这次检查本身成功了吗（网络、HTTP、解析都算）
        bool updateAvailable = false; // 查成功了，而且确实有更新的版本
        ReleaseInfo release;          // latest 那个 release（ok 为真时有效）
        Version latest;               // 从 tagName 解析出来的版本号
        Version current;              // 本程序的版本，解析失败时 parts 全是 0
        QString error;                // ok 为假时的原因，直接可以拿去给用户看
    };

    explicit UpdateChecker(QObject *parent = nullptr);
    ~UpdateChecker() override;

    // 异步。无论成败都会发一次 checkFinished。
    void check();

    // 异步下载到 destPath。进度走 downloadProgress，结束走 downloadFinished /
    // downloadFailed。校验不通过（大小或 sha256 对不上）归到 downloadFailed。
    void download(const ReleaseAsset &asset, const QString &destPath);

    // 半途放弃：中止当前请求、清掉写了半截的文件
    void abort();

    bool isBusy() const;

signals:
    void checkFinished(const ClipNote::UpdateChecker::Result &result);
    void downloadProgress(qint64 received, qint64 total);
    void downloadFinished(const QString &path);
    void downloadFailed(const QString &error);

private:
    void onCheckReplyFinished();
    // /releases/latest 回了 404 之后，再问一次仓库本身在不在，好把
    // 「仓库不存在」和「仓库有但没发过 Release」分开报
    void resolveNotFound(const Result &result);
    void onDownloadReplyFinished();
    void onDownloadReadyRead();
    void teardownDownload();

    QNetworkAccessManager m_network;

    QPointer<QNetworkReply> m_reply;
    QFile *m_sink = nullptr;

    // 下载期间的状态：一边写盘一边算 sha256，结束时和 release 里给的对一下
    QString m_destPath;
    ReleaseAsset m_asset;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    qint64 m_written = 0;
};

} // namespace ClipNote
