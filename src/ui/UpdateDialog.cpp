#include "ui/UpdateDialog.h"

#include "update/UpdateInstaller.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTextBrowser>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace ClipNote {
namespace {

// 版本号在界面上统一带 v 前缀，和 GitHub tag、关于对话框里的写法对齐。
// Version::toString() 给的是纯数字（它还要拿去拼临时目录名），前缀在这里补。
QString displayVersion(const Version &version)
{
    return QStringLiteral("v") + version.toString();
}

QString formatBytes(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

} // namespace

UpdateDialog::UpdateDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("检查更新"));
    // 面板是永远置顶的，这个对话框不跟着置顶就会被它压住
    setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint);
    setMinimumWidth(460);

    m_headline = new QLabel(this);
    m_headline->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 600; color: #1f1f1f;"));

    m_detail = new QLabel(this);
    m_detail->setWordWrap(true);
    m_detail->setStyleSheet(QStringLiteral("color: #5a5a5a;"));
    m_detail->setTextInteractionFlags(Qt::TextBrowserInteraction);

    // 更新说明直接按 markdown 渲染 —— GitHub 的 release 正文本来就是 markdown，
    // 而且它可能是用户提供的任意内容，用 QTextBrowser 显示比塞进 QLabel 稳妥
    m_notes = new QTextBrowser(this);
    m_notes->setOpenExternalLinks(true);
    m_notes->setMinimumHeight(140);
    m_notes->setStyleSheet(QStringLiteral(
        "QTextBrowser { background: #fafafa; border: 1px solid #e4e4e4; border-radius: 6px;"
        "               padding: 4px; }"));

    m_progress = new QProgressBar(this);
    m_progress->setTextVisible(true);

    m_primary = new QPushButton(this);
    m_primary->setDefault(true);

    m_secondary = new QPushButton(this);
    m_secondary->hide();

    m_close = new QPushButton(QStringLiteral("关闭"), this);
    connect(m_close, &QPushButton::clicked, this, &QDialog::reject);

    // 这两个按钮**只接一次线**，具体干什么由当前状态决定。
    //
    // 曾经的写法是在每次切状态时按需 connect，还加了 Qt::UniqueConnection 防重复 ——
    // 但那个标志对 lambda 是**无效**的（Qt 只为成员函数指针实现去重），
    // 于是「失败 → 重试 → 又失败」会让点击处理器叠好几层，
    // 用户按一下「重试」会同时发起好几个请求。
    connect(m_primary, &QPushButton::clicked, this, [this] {
        switch (m_state) {
        case State::Available:
            startDownload();
            break;
        case State::Failed:
            restart();   // 看当初是哪一步失败的，重做那一步
            break;
        default:
            break;
        }
    });
    connect(m_secondary, &QPushButton::clicked, this, [this] { openReleasePage(); });

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    buttons->addWidget(m_secondary);
    buttons->addStretch(1);
    buttons->addWidget(m_close);
    buttons->addWidget(m_primary);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(10);
    layout->addWidget(m_headline);
    layout->addWidget(m_detail);
    layout->addWidget(m_notes, 1);
    layout->addWidget(m_progress);
    layout->addLayout(buttons);

    m_checker = new UpdateChecker(this);
    connect(m_checker, &UpdateChecker::checkFinished, this, &UpdateDialog::onCheckFinished);
    connect(m_checker, &UpdateChecker::downloadProgress, this,
            [this](qint64 received, qint64 total) {
                if (total > 0) {
                    m_progress->setRange(0, 1000);
                    m_progress->setValue(static_cast<int>(received * 1000 / total));
                    m_progress->setFormat(QStringLiteral("%1 / %2")
                                              .arg(formatBytes(received), formatBytes(total)));
                } else {
                    m_progress->setRange(0, 0);   // 服务端没给长度，转成不确定状态
                }
            });
    connect(m_checker, &UpdateChecker::downloadFinished, this, &UpdateDialog::onDownloadFinished);
    connect(m_checker, &UpdateChecker::downloadFailed, this, &UpdateDialog::onDownloadFailed);

    applyState(State::Checking);
}

void UpdateDialog::startCheck()
{
    applyState(State::Checking);
    m_checker->check();
}

void UpdateDialog::reject()
{
    // 下载到一半关窗口：别让请求在后台继续跑，也别留个半截文件在临时目录里
    if (m_state == State::Downloading)
        m_checker->abort();
    QDialog::reject();
}

void UpdateDialog::applyState(State state)
{
    m_state = state;

    // 每换一次状态就先全部收起来，下面按状态挑着打开 —— 比逐个 toggle 好读，
    // 也不会漏掉某个控件忘了隐藏
    m_notes->hide();
    m_progress->hide();
    m_primary->hide();
    m_secondary->hide();
    m_primary->setEnabled(true);
    m_close->setEnabled(true);

    switch (state) {
    case State::Checking:
        m_headline->setText(QStringLiteral("正在检查更新…"));
        m_detail->setText(QStringLiteral("正在向 GitHub 查询最新版本。"));
        m_primary->hide();
        break;

    case State::UpToDate:
        m_headline->setText(QStringLiteral("已经是最新版本"));
        m_detail->setText(QStringLiteral("当前版本 %1，没有更新的版本。")
                              .arg(QCoreApplication::applicationVersion()));
        break;

    case State::Available: {
        m_headline->setText(QStringLiteral("发现新版本 %1").arg(displayVersion(m_result.latest)));
        QString detail =
            QStringLiteral("你正在用的是 %1。").arg(displayVersion(m_result.current));

        if (!m_asset) {
            // 版本是新的，但没有能自动安装的附件 —— 老实说清楚，只给「打开下载页」
            detail += QStringLiteral("\n这个 Release 里没有可以自动安装的附件"
                                     "（.zip 或 .exe），请到下载页手动获取。");
        } else if (!m_result.release.publishedAt.isNull()) {
            detail += QStringLiteral("\n发布于 %1。")
                          .arg(m_result.release.publishedAt.toLocalTime()
                                   .toString(QStringLiteral("yyyy-MM-dd")));
        }
        m_detail->setText(detail);

        if (!m_result.release.notes.trimmed().isEmpty()) {
            m_notes->setMarkdown(m_result.release.notes);
            m_notes->show();
        }

        if (m_asset) {
            m_primary->setText(QStringLiteral("下载并更新"));
            m_primary->show();
        }
        if (!m_result.release.pageUrl.isEmpty()) {
            m_secondary->setText(QStringLiteral("打开下载页"));
            m_secondary->show();
        }
        break;
    }

    case State::Downloading:
        m_headline->setText(QStringLiteral("正在下载 %1…").arg(displayVersion(m_result.latest)));
        m_detail->setText(QStringLiteral("下完会校验一遍，然后自动替换并重启。"
                                         "这期间请不要手动关掉程序。"));
        m_progress->setRange(0, 0);
        m_progress->show();
        m_primary->hide();
        m_close->setEnabled(false);   // 让「关闭」按不动，免得下到一半被误关
        break;

    case State::Failed:
        m_headline->setText(QStringLiteral("没能完成"));
        m_primary->setText(QStringLiteral("重试"));
        m_primary->show();
        break;
    }
}

void UpdateDialog::restart()
{
    if (m_retry == Retry::Download && m_asset)
        startDownload();
    else
        startCheck();
}

void UpdateDialog::onCheckFinished(const UpdateChecker::Result &result)
{
    m_result = result;
    m_asset = nullptr;

    emit checked(result.ok);

    if (!result.ok) {
        m_retry = Retry::Check;
        applyState(State::Failed);
        m_detail->setText(result.error);
        return;
    }

    if (!result.updateAvailable) {
        applyState(State::UpToDate);
        return;
    }

    m_asset = pickUpdateAsset(result.release);
    applyState(State::Available);
}

void UpdateDialog::startDownload()
{
    if (!m_asset)
        return;

    // 先确认装得下去。等下载完了才发现目录写不了，那是白等一场。
    if (!UpdateInstaller::canReplace(QCoreApplication::applicationFilePath(), nullptr)) {
        // 这条重试没用，别再下载一遍同样的结果
        m_retry = Retry::Check;
        applyState(State::Failed);
        m_detail->setText(
            QStringLiteral("ClipNote 所在的目录写不进去，没法自动更新：\n%1\n\n"
                           "装到 Program Files 之类需要管理员权限的地方就会这样。"
                           "用「打开下载页」手动换吧。")
                .arg(QDir::toNativeSeparators(
                    QFileInfo(QCoreApplication::applicationFilePath()).absolutePath())));
        m_primary->hide();
        if (!m_result.release.pageUrl.isEmpty()) {
            m_secondary->setText(QStringLiteral("打开下载页"));
            m_secondary->show();
        }
        return;
    }

    const QString workDir = UpdateInstaller::workDirFor(m_result.latest.toString());
    if (!QDir().mkpath(workDir)) {
        m_retry = Retry::Check;
        applyState(State::Failed);
        m_detail->setText(QStringLiteral("建不出临时目录：%1").arg(QDir::toNativeSeparators(workDir)));
        return;
    }

    // 文件名取附件自己的名字，后缀（.zip/.exe）后面要靠它决定怎么处理
    const QString dest = QDir(workDir).filePath(QFileInfo(m_asset->name).fileName());

    applyState(State::Downloading);
    m_checker->download(*m_asset, dest);
}

void UpdateDialog::onDownloadFinished(const QString &path)
{
    if (m_installing)
        return;

    UpdateInstaller::Prepared prepared;
    QString error;

    if (!UpdateInstaller::stage(path, m_result.latest.toString(), &prepared, &error)) {
        // 包坏了、解不开、或者里面没有主程序。再下一次可能就好了（下到一半断流），
        // 所以这里让「重试」重新下载，而不是只重查一遍版本号。
        m_retry = Retry::Download;
        applyState(State::Failed);
        m_detail->setText(QStringLiteral("准备好的文件有问题，没有替换上去：\n%1").arg(error));
        return;
    }

    if (!UpdateInstaller::launchSwap(prepared, &error)) {
        m_retry = Retry::Download;
        applyState(State::Failed);
        m_detail->setText(QStringLiteral("启动更新脚本失败，程序保持原样：\n%1").arg(error));
        return;
    }

    // 从这一刻起，替换是两个进程之间的事了。**必须马上退出** ——
    // 收尾脚本等的就是我们这个进程号，我们不走，它就一直等在那里。
    m_installing = true;
    m_headline->setText(QStringLiteral("正在更新…"));
    m_detail->setText(QStringLiteral("ClipNote 会关闭一下，换好文件之后自己启动起来。"));
    m_progress->hide();
    m_primary->hide();
    m_close->hide();

    // 先退出这个模态循环，再退出主循环。反过来的话，quit() 只叫停主循环，
    // 这个对话框自己的嵌套事件循环还在转，窗口会僵在那里。
    QTimer::singleShot(1200, this, [this] {
        accept();
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    });
}

void UpdateDialog::onDownloadFailed(const QString &error)
{
    if (m_installing)
        return;

    m_retry = Retry::Download;
    applyState(State::Failed);
    m_detail->setText(error);
}

void UpdateDialog::openReleasePage()
{
    if (!m_result.release.pageUrl.isEmpty())
        QDesktopServices::openUrl(QUrl(m_result.release.pageUrl));
}

} // namespace ClipNote
