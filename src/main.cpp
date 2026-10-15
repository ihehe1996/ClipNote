// ClipNote —— 剪贴板笔记
//
// 程序形态是「托盘常驻」：启动时把面板显示出来，收起后只留一个托盘图标，
// 之后按全局热键唤出。因此这里显式关掉了「最后一个窗口关闭就退出」的默认行为，
// 退出统一走托盘菜单。

#include "AppConfig.h"
#include "SelfTest.h"
#include "model/ClipStore.h"
#include "model/ClipboardWatcher.h"
#include "platform/GlobalHotkey.h"
#include "ui/MainWindow.h"
#include "ui/Tray.h"
#include "update/UpdateInstaller.h"

#include <QApplication>
#include <QKeySequence>
#include <QTimer>

#include <memory>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ClipNote"));
    app.setApplicationDisplayName(QStringLiteral("ClipNote"));
    app.setOrganizationName(QStringLiteral("ClipNote"));
    // 版本号来自 CMakeLists.txt，改那一行就够了（见 AppConfig.h 的 appVersion）
    app.setApplicationVersion(ClipNote::appVersion());

    // 关掉最后一个窗口不等于退出 —— 托盘常驻的程序要活到用户主动退出
    app.setQuitOnLastWindowClosed(false);

    // 自检模式：不开窗口，跑完就退，结果写在当前目录的 selftest.log。
    // 必须在构造 ClipStore / ClipboardWatcher 之前返回 —— 自检会往剪贴板里写一个哨兵值
    // 再还原，要是那时已经有 watcher 在跑，哨兵会被当成真实内容收进历史。
    if (ClipNote::isSelfTestRequest(app.arguments()))
        return ClipNote::runSelfTest(app.arguments());

    // 上次更新失败会留下临时目录（里面有日志），过期的顺手清掉
    ClipNote::UpdateInstaller::cleanStaleWorkDirs();

    ClipNote::ClipStore store;
    ClipNote::ClipboardWatcher watcher(&store);

    ClipNote::MainWindow window(&store, &watcher);

    ClipNote::Tray tray;
    QObject::connect(&tray, &ClipNote::Tray::toggleRequested, &window, &ClipNote::MainWindow::toggle);
    QObject::connect(&tray, &ClipNote::Tray::quitRequested, &app, &QApplication::quit);
    QObject::connect(&tray, &ClipNote::Tray::checkUpdateRequested,
                     &window, &ClipNote::MainWindow::checkForUpdates);
    tray.setUpdateActionVisible(!ClipNote::updateRepo().isEmpty());
    tray.show();

    // 启动就把面板显示出来，不用用户再去按热键。
    // （托盘不可用时更是必须 —— 否则一个入口都没有，程序会变成看不见的进程。）
    //
    // 唯一的例外是开机自启（--silent，由安装程序写进 HKCU 的 Run 项）：
    // 那种场合弹面板就是打扰了，安静地驻留托盘即可。
    if (!ClipNote::isSilentStartRequested(app.arguments()))
        window.showPanel();

    // 全局热键。注册失败不是致命错误：托盘菜单照样能用，只是少了个快捷键，
    // 所以提示一下就好，不要退出。
    ClipNote::GlobalHotkey hotkey;
    const QKeySequence shortcut(ClipNote::defaultHotkey());
    const QString shortcutText = shortcut.toString(QKeySequence::PortableText);

    if (hotkey.grab(window.winId(), shortcut)) {
        QObject::connect(&hotkey, &ClipNote::GlobalHotkey::activated,
                         &window, &ClipNote::MainWindow::toggle);
        tray.setHotkeyText(shortcutText);
    } else {
        tray.setHotkeyText(QStringLiteral("注册失败"));
        tray.showMessage(QStringLiteral("ClipNote"),
                         QStringLiteral("全局热键 %1 没能注册：%2\n托盘菜单仍然可用。")
                             .arg(shortcutText, hotkey.errorString()));
    }

    // 启动时悄悄查一次更新（有节流，见 AppConfig.h）。
    // 查到只弹个气泡，不弹对话框 —— 用户可能正在干别的，别打断他。
    //
    // notifiedWithUpdate 这个标志是用来区分「用户点的是哪个气泡」的：
    // 热键注册失败也会弹气泡，那种时候点一下不该冒出更新对话框。
    auto notifiedWithUpdate = std::make_shared<bool>(false);

    QObject::connect(&window, &ClipNote::MainWindow::updateAvailable, &tray,
                     [&tray, notifiedWithUpdate](const QString &version) {
                         *notifiedWithUpdate = true;
                         // 气泡上只放得下两行，说明白「点它就能装」就够了
                         tray.showMessage(
                             QStringLiteral("ClipNote 有新版本 %1").arg(version),
                             QStringLiteral("当前是 %1。点这条消息可以下载并更新。")
                                 .arg(QCoreApplication::applicationVersion()));
                     });

    QObject::connect(&tray, &ClipNote::Tray::messageClicked, &window,
                     [&window, notifiedWithUpdate] {
                         if (!*notifiedWithUpdate)
                             return;
                         *notifiedWithUpdate = false;
                         window.checkForUpdates();
                     });

    QTimer::singleShot(ClipNote::updateStartupDelayMs(), &window,
                       &ClipNote::MainWindow::startSilentUpdateCheck);

    return app.exec();
}
