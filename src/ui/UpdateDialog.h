#pragma once

#include "update/UpdateChecker.h"

#include <QDialog>

QT_BEGIN_NAMESPACE
class QLabel;
class QProgressBar;
class QPushButton;
class QTextBrowser;
QT_END_NAMESPACE

namespace ClipNote {

// 「检查更新」的对话框，顺带把整条流程串起来：查 → 问用户 → 下载 → 交给
// UpdateInstaller 换文件 → 退出重启。
//
// 没有做成一个单纯的「显示层」再配一个控制器：这四步的状态迁移就是这么一张
// 状态机，拆成两个对象反而要来回传状态。
//
// 它自己**不查也不下载**，那是 UpdateChecker 的活；这里只决定什么时候调它、
// 以及拿到结果之后给用户看什么。
class UpdateDialog : public QDialog
{
    Q_OBJECT

public:
    explicit UpdateDialog(QWidget *parent = nullptr);

    // 打开就查（除非调用方设了别的）
    void startCheck();

signals:
    // 一次检查跑完了。ok 说的是「这次检查本身成不成功」，和有没有新版本无关 ——
    // 调用方靠它决定要不要更新「上次检查时间」来给下次静默检查节流。
    void checked(bool ok);

protected:
    void reject() override;   // 下载中途关窗口要中止请求

private:
    enum class State { Checking, UpToDate, Available, Downloading, Failed };
    // 「重试」要重做哪一步。失败的可能是查询，也可能是下载，两者差得远。
    enum class Retry { Check, Download };

    void applyState(State state);
    void restart();
    void onCheckFinished(const UpdateChecker::Result &result);
    void startDownload();
    void onDownloadFinished(const QString &path);
    void onDownloadFailed(const QString &error);
    void openReleasePage();

    UpdateChecker *m_checker = nullptr;

    State m_state = State::Checking;
    Retry m_retry = Retry::Check;
    UpdateChecker::Result m_result;
    const ReleaseAsset *m_asset = nullptr;   // 指向 m_result.release.assets 里的元素
    bool m_installing = false;               // 已经交给收尾脚本了，别再当失败处理

    QLabel *m_headline = nullptr;
    QLabel *m_detail = nullptr;
    QTextBrowser *m_notes = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_primary = nullptr;
    QPushButton *m_secondary = nullptr;
    QPushButton *m_close = nullptr;
};

} // namespace ClipNote
