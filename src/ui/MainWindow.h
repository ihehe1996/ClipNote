#pragma once

#include "platform/WindowResizer.h"   // m_resizer 是值成员，这里得要完整类型

#include <QModelIndex>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QEvent;
class QLabel;
class QListView;
class QResizeEvent;
class QTimer;
QT_END_NAMESPACE

namespace ClipNote {

class ClipStore;
class ClipboardWatcher;
class TitleBar;
class PreviewPopup;
class UpdateChecker;

// 剪贴板面板。
//
// 关键性质：**这个面板不抢焦点**。用的是 Qt::WindowDoesNotAcceptFocus，
// 在 Windows 上就是 WS_EX_NOACTIVATE —— 和系统自带的剪贴板（Win+V）一个路子。
// 用户按热键唤出它时，原来那个程序的输入框连光标都不会闪，他能接着打字。
//
// 由此带来两点，都是刻意的：
//
// 1. 选中一条时**不需要任何焦点归还操作**。焦点从头到尾都在用户原来的程序上，
//    所以直接往当前焦点窗口发一个 Ctrl+V 就落在正确的地方。
//    （曾经为此写过一整套「记住前台窗口 → SetForegroundWindow → AttachThreadInput
//    → 等待焦点落地」，改成非激活面板后这些全部删掉了，连带删掉了
//    platform/WindowTarget 整个文件。）
// 2. 面板**收不到任何键盘输入** —— 没有搜索框，也不能用方向键，只能鼠标点选。
//    这是换来「不抢焦点」的代价，和 Win+V 是同一个取舍。
class MainWindow : public QWidget
{
    Q_OBJECT

public:
    // watcher 用来告知「这次剪贴板变化是我们自己写的」—— 见 pasteRow()。
    explicit MainWindow(ClipStore *store, ClipboardWatcher *watcher, QWidget *parent = nullptr);

public slots:
    // 热键和托盘菜单都接这一个 —— 面板不记任何「目标窗口」状态，
    // 两条路径的行为完全一样，不需要分开。
    void toggle();
    void showPanel();
    void hidePanel();

    // 齿轮菜单里的「检查更新…」。打开的对话框自己负责查和下载。
    void checkForUpdates();

    // 启动时那一次静默检查。每次启动都查（没有节流，理由见 AppConfig.h），
    // 查到新版本直接把更新对话框摆出来。
    void startSilentUpdateCheck();

protected:
    void closeEvent(QCloseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    // 拖边缘改大小全靠原生消息，见 platform/WindowResizer.h
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
    void resizeEvent(QResizeEvent *event) override;
    // 悬停提示要盯着列表视口：视图本身没有「鼠标在这一格里挪了挪」这种信号
    //（entered() 只在换行的时候发），而且 Qt 自动弹气泡的时机也要在这里接管
    //（它的延迟没有公开接口，见 eventFilter 里的说明）。
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void positionNearCursor();
    void updateEmptyState();
    void pasteRow(const QModelIndex &index);
    void showContextMenu(const QPoint &pos);
    void updatePreviewFor(const QPoint &viewportPos);
    void showPreview();
    void hidePreview();

    ClipStore *m_store = nullptr;
    ClipboardWatcher *m_watcher = nullptr;
    QWidget *m_owner = nullptr;   // 永远隐藏的归属窗口，作用是让面板不进任务栏
    TitleBar *m_titleBar = nullptr;   // 自绘标题栏（系统的没有，见 TitleBar.h）
    QListView *m_list = nullptr;
    QLabel *m_empty = nullptr;

    WindowResizer m_resizer{this};   // 拖边缘改大小（无边框窗口得自己补回来）

    // 悬停预览：文本停一会儿弹全文，图片停在缩略图上弹大图。
    PreviewPopup *m_preview = nullptr;
    QTimer *m_previewTimer = nullptr;
    int m_previewRow = -1;          // 光标当前停在第几行，-1 = 不在任何一行上
    bool m_previewOnThumb = false;  // 光标是不是正压在那一行的缩略图上

    // 尺寸变了要记进 QSettings，但拖动时 resizeEvent 是**每帧**一次，
    // 不能每帧都去写一遍注册表 —— 停手之后再落盘。
    QTimer *m_saveSizeTimer = nullptr;

    // 静默检查专用。手动检查用的是对话框里那个，两者互不干扰 ——
    // 共用一个的话，用户手点「检查更新」会把启动时那次请求顶掉。
    UpdateChecker *m_silentChecker = nullptr;
};

} // namespace ClipNote
