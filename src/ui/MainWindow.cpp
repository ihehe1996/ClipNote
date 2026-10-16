#include "ui/MainWindow.h"

#include "AppConfig.h"
#include "model/ClipStore.h"
#include "model/ClipboardWatcher.h"
#include "platform/AutoPaste.h"
#include "ui/AboutDialog.h"
#include "ui/ClipDelegate.h"
#include "ui/TitleBar.h"
#include "ui/UpdateDialog.h"
#include "update/UpdateChecker.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCursor>
#include <QGuiApplication>
#include <QImage>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

namespace ClipNote {

namespace {

// 上次用的面板尺寸。存过的值要夹两道，因为它可能是「换过显示器之后已经放不下的旧值」：
// 先不小于最小尺寸，再不超过当前屏幕的可用区域，最后再兜一次下限（屏幕比最小尺寸还小的
// 极端情况下，宁可超出屏幕也要保住排版）。
QSize restoredPanelSize()
{
    QSize size = QSettings().value(QStringLiteral("panel/size")).toSize();
    if (size.isEmpty())
        size = defaultPanelSize();

    size = size.expandedTo(minimumPanelSize());
    if (QScreen *screen = QGuiApplication::primaryScreen())
        size = size.boundedTo(screen->availableGeometry().size());
    return size.expandedTo(minimumPanelSize());
}

} // namespace

MainWindow::MainWindow(ClipStore *store, ClipboardWatcher *watcher, QWidget *parent)
    : QWidget(parent), m_store(store), m_watcher(watcher)
{
    setWindowTitle(QStringLiteral("ClipNote"));

    // WindowDoesNotAcceptFocus 对应 Win32 的 WS_EX_NOACTIVATE：本窗口永远不成为活动
    // 窗口，连点它也不会 —— 这就是「不抢焦点」的实现方式，和系统剪贴板一致。
    // StaysOnTop 让它盖在用户正在打字的那个程序上面。
    //
    // FramelessWindowHint：标题栏是自绘的（见 TitleBar.h）。Windows 11 的标题栏由 DWM
    // 合成，应用在 WM_NCPAINT 里画上去的东西会被盖掉（实测过），所以想在标题栏上多放
    // 一颗按钮就只能整条自己画，窗口也就没有系统边框了。
    //
    // 代价：没有系统的拖拽调大小（自己补回来了，见 platform/WindowResizer.h）；
    // 也没有原生投影。
    const Qt::WindowFlags flags = Qt::Window | Qt::FramelessWindowHint
                                  | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus;

    // 永远隐藏的归属窗口。Windows 规定「有 owner 的窗口不进任务栏」，靠它让面板
    // 不在任务栏里冒出按钮。
    // 只取 winId() 不 show()：它要是真显示出来反而会自己占一个任务栏位置。
    m_owner = new QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint);
    m_owner->setAttribute(Qt::WA_DontShowOnScreen);
    // 起个名字：不给的话它会继承应用名，和面板的标题撞成一样，外部检查脚本就分不出
    // 哪个窗口是面板了（tools/check-panel.py 靠标题找面板）。隐藏窗口的标题没人看得见。
    m_owner->setWindowTitle(QStringLiteral("ClipNote 隐藏归属窗口"));
    m_owner->winId(); // 先把原生窗口建出来，它要作为 owner
    // 注意：下面这行 setParent 同时会建立 QObject 父子关系，所以 m_owner 成了本窗口的
    // 「父对象」。**不要在析构里 delete m_owner** —— 那会把本窗口一起删掉。它就是进程
    // 生命周期内的一个对象，退出时由系统回收。
    //
    // 用 setParent 挂 owner（也就是 Qt 的 transient parent），**不要**手工去设
    // GWLP_HWNDPARENT：Qt 在 show() 时会重新应用一遍窗口属性，会把手工设的冲掉，
    // 实测 GW_OWNER 会变回 0，面板照样进任务栏。
    setParent(m_owner, flags);
    // 显示时也不激活。和上面的 flag 一起用，保险。
    setAttribute(Qt::WA_ShowWithoutActivating);

    // 尺寸可以拖边缘改（见 platform/WindowResizer.h），所以这里只用记着的那个尺寸，
    // 没有就退回默认值。**位置不记** —— 它是 positionNearCursor() 每次唤出时按鼠标现算的。
    setMinimumSize(minimumPanelSize());
    resize(restoredPanelSize());

    m_saveSizeTimer = new QTimer(this);
    m_saveSizeTimer->setSingleShot(true);
    m_saveSizeTimer->setInterval(400);
    connect(m_saveSizeTimer, &QTimer::timeout, this, [this] {
        QSettings().setValue(QStringLiteral("panel/size"), size());
    });

    m_titleBar = new TitleBar(this);
    connect(m_titleBar, &TitleBar::closeRequested, this, &MainWindow::hidePanel);
    connect(m_titleBar, &TitleBar::checkUpdateRequested, this, &MainWindow::checkForUpdates);
    connect(m_titleBar, &TitleBar::aboutRequested, this, [this] {
        // 栈上开、模态跑。这样生命周期不用管：exec() 一回来它就没了。
        AboutDialog dialog(this);
        dialog.exec();
    });

    m_list = new QListView(this);
    m_list->setModel(m_store);
    m_list->setItemDelegate(new ClipDelegate(m_list));   // 每行的画法见 ClipDelegate
    m_list->setUniformItemSizes(true);
    m_list->setTextElideMode(Qt::ElideRight);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setFrameShape(QFrame::NoFrame);
    // 自绘 delegate 靠 State_MouseOver 判断悬停，得让视图跟踪鼠标
    m_list->setMouseTracking(true);

    QFont listFont = m_list->font();
    listFont.setPixelSize(13);
    m_list->setFont(listFont);

    // 列表本身透明（底色由 body 提供），滚动条改成细的 —— 默认那条太粗，很抢眼
    m_list->setStyleSheet(QStringLiteral(
        "QListView { background: transparent; border: none; outline: none; }"
        "QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }"
        "QScrollBar::handle:vertical { background: #cfcfcf; border-radius: 4px; min-height: 28px; }"
        "QScrollBar::handle:vertical:hover { background: #b0b0b0; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }"));

    m_empty = new QLabel(this);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_empty->setStyleSheet(QStringLiteral("color: #9a9a9a;"));

    auto *body = new QWidget(this);
    body->setObjectName(QStringLiteral("clipNoteBody"));
    body->setStyleSheet(QStringLiteral("QWidget#clipNoteBody { background: #ffffff; }"));
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(6, 6, 6, 6);
    bodyLayout->addWidget(m_empty, 1);
    bodyLayout->addWidget(m_list, 1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_titleBar);
    layout->addWidget(body, 1);

    // clicked 是单击就发的信号。面板收不到键盘，用不了 activated（那个靠回车或双击），
    // 单击是这里唯一的操作方式。
    connect(m_list, &QListView::clicked, this, &MainWindow::pasteRow);

    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListView::customContextMenuRequested, this, &MainWindow::showContextMenu);

    connect(m_store, &QAbstractItemModel::rowsInserted, this, &MainWindow::updateEmptyState);
    connect(m_store, &QAbstractItemModel::rowsRemoved, this, &MainWindow::updateEmptyState);
    connect(m_store, &QAbstractItemModel::modelReset, this, &MainWindow::updateEmptyState);

    // 刻意**没有**「点面板外面就收起」。
    //
    // 曾经做过一版：面板不抢焦点，收不到 focusOutEvent，就靠一个 100ms 的定时器轮询
    // 鼠标按键、判断点是不是落在面板矩形之外。实测很难用 —— 正因为面板不抢焦点，
    // 用户点自己那个程序（比如想把光标挪到要粘贴的位置）是完全正常的操作，
    // 结果一点面板就没了，想连点几条也做不到。
    //
    // 现在面板只在三种情况下收起：再按一次热键、点标题栏的 ✕、走托盘菜单。

    updateEmptyState();
}

void MainWindow::toggle()
{
    // 面板是**永远置顶**的（Qt::WindowStaysOnTopHint + WS_EX_TOPMOST），所以
    // 「开着但被压在下面」这个状态不存在，toggle 就是单纯的显示/隐藏。
    //
    // 这里曾经有过一版三分支的（收起着 → 唤出；已在最前 → 收起；被压住 → 提到前面），
    // 那是为了配合标题栏上那颗「总在最前」图钉。图钉去掉了，那套也就没必要了。
    if (isVisible())
        hidePanel();
    else
        showPanel();
}

void MainWindow::showPanel()
{
    positionNearCursor();

    // Qt::WindowDoesNotAcceptFocus 在本机不会真的落到窗口样式上，得手工补
    // WS_EX_NOACTIVATE（详见 AutoPaste.h）。得在 show() 之前补。
    makeWindowNonActivating(winId());
    // 无边框窗口 DWM 默认不给圆角，四个直角太扎眼
    setWindowRoundedCorners(winId());

    show();
    raise();
    // 刻意不调 activateWindow() —— 那会把焦点从用户原来的程序上抢过来，
    // 正是这个面板要避免的事。

    updateEmptyState();
}

void MainWindow::hidePanel()
{
    hide();
}

void MainWindow::checkForUpdates()
{
    UpdateDialog dialog(this);
    dialog.startCheck();
    dialog.exec();
}

void MainWindow::startSilentUpdateCheck()
{
    // 没配更新源就别发请求了。齿轮菜单里手点会给出明确提示，启动时不用打扰。
    if (updateRepo().isEmpty())
        return;

    if (!m_silentChecker) {
        m_silentChecker = new UpdateChecker(this);
        connect(m_silentChecker, &UpdateChecker::checkFinished, this,
                [this](const UpdateChecker::Result &result) {
                    // 只有真查到新版本才弹窗：查失败、已是最新都不该无端拦人。
                    //
                    // 这里不用托盘气泡 —— 气泡属于系统通知，用户能在系统设置里整个关掉、
                    // 专注助手也会把它压掉，而且**失败时不报错**，那条路一断用户永远
                    // 不知道有新版本。对话框是自己画的，一定会出现。
                    if (!result.ok || !result.updateAvailable)
                        return;
                    UpdateDialog dialog(this);
                    dialog.showResult(result);   // 结果现成的，别再查一遍
                    dialog.exec();
                });
    }

    m_silentChecker->check();
}

void MainWindow::pasteRow(const QModelIndex &index)
{
    if (!index.isValid())
        return;

    // 先把内容取到局部变量再动剪贴板：写剪贴板会让 store 把这一条挪到第 0 条，
    // 之后 index 就失效了，不能握着它跨过这一步。
    //
    // 同时先告诉 watcher「下面这次剪贴板变化是我们自己写的」：否则它会当成用户新复制的
    // 内容，触发 store 把这一条挪到列表最前面 —— 面板并不关闭，列表就在用户眼皮底下
    // 重排，他下一次点击会落到别的条目上。
    if (index.data(ClipStore::KindRole).toInt() == int(Clip::Image)) {
        const QImage image = index.data(ClipStore::ImageRole).value<QImage>();
        if (image.isNull())
            return;
        if (m_watcher)
            m_watcher->suppressImageChange(ClipStore::imageHash(image));
        QGuiApplication::clipboard()->setImage(image);
    } else {
        const QString text = index.data(ClipStore::TextRole).toString();
        if (text.isEmpty())
            return;
        if (m_watcher)
            m_watcher->suppressChange(text);
        QGuiApplication::clipboard()->setText(text);
    }

    // 焦点一直在用户原来的程序上，所以这一下直接落在正确的位置，不需要任何焦点归还。
    if (autoPasteEnabled())
        pasteIntoFocusedWindow();

    // 刻意不收起面板：用户可以连着点好几条。
    // 收起的途径是：再按一次热键、点标题栏的 ✕、或走托盘菜单。
}

void MainWindow::showContextMenu(const QPoint &pos)
{
    const QModelIndex index = m_list->indexAt(pos);

    QMenu menu;
    // 点在空白处就没有「这一条」可删，只给清空
    QAction *removeAction = index.isValid()
                                ? menu.addAction(QStringLiteral("删除这一条"))
                                : nullptr;
    QAction *clearAction = menu.addAction(QStringLiteral("清空全部历史"));

    QAction *chosen = menu.exec(m_list->viewport()->mapToGlobal(pos));

    if (!chosen)
        return;

    if (chosen == removeAction)
        m_store->removeAt(index.row());
    else if (chosen == clearAction)
        m_store->clear();
}

void MainWindow::positionNearCursor()
{
    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;

    const QRect area = screen->availableGeometry();
    const QPoint cursor = QCursor::pos();

    // 出现在鼠标附近，但夹在屏幕可用区域内。
    // 上界取 max(左/上, 右/下 - 尺寸)，免得窗口比屏幕还大时 qBound 的上下界翻转。
    const int maxX = qMax(area.left(), area.right() - width());
    const int maxY = qMax(area.top(), area.bottom() - height());

    move(qBound(area.left(), cursor.x() - width() / 2, maxX),
         qBound(area.top(), cursor.y() - 40, maxY));
}

void MainWindow::updateEmptyState()
{
    const bool hasRows = m_store->rowCount() > 0;

    m_list->setVisible(hasRows);
    m_empty->setVisible(!hasRows);

    if (!hasRows)
        m_empty->setText(QStringLiteral("还没有剪贴板历史。\n\n复制点什么，这里就会出现。"));
}

void MainWindow::paintEvent(QPaintEvent *)
{
    // 无边框窗口既没有系统描边也没有投影，自己画一圈，
    // 免得面板和它底下的窗口糊在一起分不出边界。
    // 圆角要和 setWindowRoundedCorners 请求的半径对得上，不然四个角会露出直角边。
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QColor(0xCF, 0xCF, 0xCF));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // 托盘程序里「关窗口」= 收起来，不是退出。退出走托盘菜单。
    event->ignore();
    hidePanel();
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    // 拖边缘改大小全靠原生消息 —— 无边框窗口没有系统缩放边框，而这块**不能用 Qt 的
    // 鼠标事件**来做：布局是 setContentsMargins(0,0,0,0)，标题栏和 body 铺满整个矩形，
    // 边缘那几像素属于子控件，Qt 的鼠标事件又不往上冒泡，MainWindow 根本收不到。
    // 原生命中测试是窗口级的，不受子控件影响。
    if (m_resizer.handleMessage(eventType, message, result))
        return true;

    return QWidget::nativeEvent(eventType, message, result);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);

    // 记下来，但延迟落盘：拖动过程中这个函数每帧都跑一次，不能每帧写一遍注册表。
    // 用重启式单次定时器，手一停就写。
    m_saveSizeTimer->start();
}

} // namespace ClipNote
