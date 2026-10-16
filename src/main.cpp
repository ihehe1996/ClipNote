// ClipNote —— 剪贴板笔记
//
// 程序形态是「托盘常驻」：启动时把面板显示出来，收起后只留一个托盘图标，
// 之后按全局热键唤出。因此这里显式关掉了「最后一个窗口关闭就退出」的默认行为，
// 退出统一走托盘菜单。

#include "AppConfig.h"
#include "SelfTest.h"
#include "model/ClipHistoryFile.h"
#include "model/ClipImageFile.h"
#include "model/ClipStore.h"
#include "model/ClipboardWatcher.h"
#include "platform/GlobalHotkey.h"
#include "ui/AppIcon.h"
#include "ui/MainWindow.h"
#include "ui/Tray.h"
#include "update/UpdateInstaller.h"

#include <QApplication>
#include <QKeySequence>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ClipNote"));
    app.setApplicationDisplayName(QStringLiteral("ClipNote"));
    app.setOrganizationName(QStringLiteral("ClipNote"));
    // 版本号来自 CMakeLists.txt，改那一行就够了（见 AppConfig.h 的 appVersion）
    app.setApplicationVersion(ClipNote::appVersion());
    // 「检查更新」和「关于」用的是系统标题栏，左上角那颗图标默认是 Windows 给的
    // 空白图标 —— 设了应用程序图标它们才会跟着变。面板自己是自绘标题栏，用不到这个；
    // 托盘图标另行设置（见 ui/Tray）。
    app.setWindowIcon(ClipNote::appIcon());

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
    // 图片落盘到哪。**必须在 loadHistory 之前设** —— 读历史时要靠它去读缩略图。
    store.setImagesDir(ClipNote::defaultImagesDir());

    // 先把上次的历史装回来，**再**建 watcher：watcher 构造时会采样一次当前剪贴板，
    // 那是要的 —— 你最后复制的东西本来就该在最前面；要是它已经在历史里，
    // pushText 会去重并置顶，不会多出一条。
    // 读失败（第一次跑、文件坏了）就当空历史启动，不打扰用户。
    {
        QString error;
        ClipNote::loadHistory(ClipNote::historyFilePath(), &store, &error);
    }

    // 扫掉没人引用的图片文件。这是兜底：程序要是在「写完图片」和「写 history.json」
    // 之间崩掉（或者被强杀），就会留下一个永远没人引用的图片文件占着磁盘。
    ClipNote::removeOrphanImages(store.imagesDir(), store.liveImageHashes());

    // 退出前补存一次：写盘是防抖的，刚复制完就退出的话定时器还没到点，那条就丢了。
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [&store] {
        QString error;
        ClipNote::saveHistory(ClipNote::historyFilePath(), store, &error);
    });

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

    // 启动时查一次更新。查到新版本由 MainWindow 直接把对话框摆出来
    //（不再走托盘气泡 —— 系统通知会被关掉，而且失败时悄无声息）。
    // 延迟这一会儿是为了别跟托盘、热键、面板抢时间。
    QTimer::singleShot(ClipNote::updateStartupDelayMs(), &window,
                       &ClipNote::MainWindow::startSilentUpdateCheck);

    return app.exec();
}
