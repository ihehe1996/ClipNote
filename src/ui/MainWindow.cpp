#include "ui/MainWindow.h"

#include "AppConfig.h"
#include "model/ClipFilterProxy.h"
#include "model/ClipHistoryFile.h"
#include "model/ClipStore.h"
#include "model/ClipboardWatcher.h"
#include "platform/AutoPaste.h"
#include "ui/AboutDialog.h"
#include "ui/ClipDelegate.h"
#include "ui/PreviewPopup.h"
#include "ui/TitleBar.h"
#include "ui/UpdateDialog.h"
#include "update/UpdateChecker.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCursor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QTabBar>
#include <QTimer>
#include <QToolTip>
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
    // 原生窗口标题**故意**不带版本号 —— 用户看到的那行字是自绘标题栏里那个 QLabel
    //（TitleBar，带版本号）。两个原因：
    //   1. 它跟应用的显示名（main.cpp 的 setApplicationDisplayName）一模一样，而 Qt 只在
    //      标题**不以显示名结尾**时才追加「 - ClipNote」。所以现在枚举到的窗口标题干干净净
    //      就是「ClipNote」；改成带版本号反而会变成「ClipNote v1.0.1 - ClipNote」。
    //   2. tools/check-panel.py 靠这个标题从进程外认面板（隐藏 owner 用的是另一个标题）。
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

    // 视图挂的是**代理**：顶部两个选项卡靠它过滤。索引语义见下面 pasteRow() 上面那段。
    m_proxy = new ClipFilterProxy(this);
    m_proxy->setSourceModel(m_store);

    m_list = new QListView(this);
    m_list->setModel(m_proxy);
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

    // 「历史记录 / 收藏」。放在 body 里而不是标题栏和 body 之间：它是白色内容区的
    // 一部分，这样能直接继承 body 的白底和 6px 边距，不用自己再对一套配色。
    m_tabs = new QTabBar(this);
    m_tabs->setObjectName(QStringLiteral("clipNoteTabs"));
    m_tabs->addTab(QStringLiteral("历史记录"));
    m_tabs->addTab(QStringLiteral("收藏"));
    // 面板收不到键盘，别让选项卡去抢焦点
    m_tabs->setFocusPolicy(Qt::NoFocus);
    m_tabs->setDrawBase(false);   // 不要那条贯穿整行的底线，只留选中项下面的
    m_tabs->setExpanding(false);
    m_tabs->setStyleSheet(QStringLiteral(
        "QTabBar#clipNoteTabs { background: transparent; }"
        "QTabBar#clipNoteTabs::tab { background: transparent; border: none;"
        "                              padding: 5px 2px; margin-right: 16px; color: #6b6b6b; }"
        "QTabBar#clipNoteTabs::tab:selected { color: #1f1f1f;"
        "                                     border-bottom: 2px solid #2F6FED; }"
        "QTabBar#clipNoteTabs::tab:hover:!selected { color: #3a3a3a; }"));

    auto *body = new QWidget(this);
    body->setObjectName(QStringLiteral("clipNoteBody"));
    body->setStyleSheet(QStringLiteral("QWidget#clipNoteBody { background: #ffffff; }"));
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(6, 6, 6, 6);
    // 选项卡居中。QTabBar 自己只会左对齐，两侧各放一个弹簧把它顶到中间 ——
    // 别改用 setExpanding(true)，那是把每个选项卡**拉宽**铺满整行，不是居中。
    auto *tabRow = new QHBoxLayout;
    tabRow->addStretch(1);
    tabRow->addWidget(m_tabs);
    tabRow->addStretch(1);
    bodyLayout->addLayout(tabRow);
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

    // 悬停预览。视图为了悬停高亮已经开了鼠标跟踪，所以 MouseMove 是有的；
    // 但「鼠标在这一行里挪了挪」没有对应信号（entered() 只在换行时发），
    // 得自己盯视口的事件，见 eventFilter()。
    m_preview = new PreviewPopup(this);
    m_previewTimer = new QTimer(this);
    m_previewTimer->setSingleShot(true);
    connect(m_previewTimer, &QTimer::timeout, this, &MainWindow::showPreview);
    m_list->viewport()->installEventFilter(this);

    connect(m_tabs, &QTabBar::currentChanged, this, &MainWindow::onTabChanged);

    // 空状态接的是**代理**的信号，不是源模型的：接源模型的话，源一发信号就可能先跑到
    // 这里，而代理内部的行数映射还没更新，读到的 rowCount 是旧的。
    connect(m_proxy, &QAbstractItemModel::rowsInserted, this, &MainWindow::updateEmptyState);
    connect(m_proxy, &QAbstractItemModel::rowsRemoved, this, &MainWindow::updateEmptyState);
    connect(m_proxy, &QAbstractItemModel::modelReset, this, &MainWindow::updateEmptyState);

    // 历史落盘：模型变了就重启定时器，停手 1 秒才写。接的是**源模型** ——
    // 落盘关心的是全部历史，跟当前在看哪个选项卡无关。
    m_saveHistoryTimer = new QTimer(this);
    m_saveHistoryTimer->setSingleShot(true);
    m_saveHistoryTimer->setInterval(historySaveDelayMs());
    connect(m_saveHistoryTimer, &QTimer::timeout, this, [this] {
        QString error;
        saveHistory(historyFilePath(), *m_store, &error);
    });

    connect(m_store, &QAbstractItemModel::rowsInserted, this, &MainWindow::scheduleHistorySave);
    connect(m_store, &QAbstractItemModel::rowsRemoved, this, &MainWindow::scheduleHistorySave);
    connect(m_store, &QAbstractItemModel::modelReset, this, &MainWindow::scheduleHistorySave);
    connect(m_store, &QAbstractItemModel::dataChanged, this, &MainWindow::scheduleHistorySave);

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
    hidePreview();
    // 索引也要清掉：面板收起来再唤出时鼠标可能没动过，不清的话
    // 那次 MouseMove 会被当成「还在原来那一格」而直接返回，预览就再也弹不出来了。
    m_previewIndex = QPersistentModelIndex();
    m_previewOnThumb = false;

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

// 索引语义（加了过滤代理之后，这块是最容易错的地方）：
//
//   视图吐出来的 index —— clicked 的参数、indexAt()、visualRect() 的入参 ——
//   **一律是代理索引**。取数据可以直接 index.data(...)，代理会自动转发到源模型。
//
//   **千万别写 m_store->data(代理索引)**：它不做映射，会把代理行号当源行号用。
//   历史页不过滤、行号恰好重合，看着一切正常；一切到收藏页就静默取错条目 ——
//   不崩、不报错，预览显示成另一条的、右键删除删掉另一条。
//
//   反过来，凡是喂给源模型的行号（removeAt / setFavorite / isFavorite）必须走
//   m_proxy->mapToSource(index).row()。
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
    // 持久索引：menu.exec() 是模态的，但**它会跑事件循环**，其间剪贴板推进来一条
    // 新内容就可能把源模型重排掉，裸 QModelIndex 会失效，回来再删就删错行了。
    const QPersistentModelIndex index = m_list->indexAt(pos);

    QMenu menu;
    QAction *favoriteAction = nullptr;
    QAction *removeAction = nullptr;
    QAction *clearAction = nullptr;
    QAction *unfavoriteAllAction = nullptr;

    if (index.isValid()) {
        // 点在条目上：只给和「这一条」有关的动作，**不出现清空**
        favoriteAction = menu.addAction(index.data(ClipStore::FavoriteRole).toBool()
                                            ? QStringLiteral("取消收藏")
                                            : QStringLiteral("收藏"));
        removeAction = menu.addAction(QStringLiteral("删除这一条"));
    } else {
        // 点在空白处。清空保留收藏 —— 收藏是特意留下来的东西，被一刀切掉就没意义了。
        clearAction = menu.addAction(QStringLiteral("清空历史（保留收藏）"));
        unfavoriteAllAction = menu.addAction(QStringLiteral("取消全部收藏"));
    }

    QAction *chosen = menu.exec(m_list->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;

    // 菜单关掉之后那个索引可能已经失效（见上面那条注释），映射成源行号之前再确认一次
    const QModelIndex source = index.isValid() ? m_proxy->mapToSource(index) : QModelIndex();

    if (chosen == favoriteAction) {
        if (source.isValid())
            m_store->setFavorite(source.row(), !m_store->isFavorite(source.row()));
        // 在收藏页取消收藏，这一行得立刻从列表里消失 —— 代理不会自己重评估。
        // 放在 dataChanged 的槽里做会重入，所以由调用方显式刷。
        m_proxy->refresh();
        // refresh() 到底发 rowsRemoved 还是只有 layoutChanged 不值得赌，
        // 而空状态是靠那些信号更新的 —— 取消掉收藏页最后一条时要能切到空状态文案
        updateEmptyState();
    } else if (chosen == removeAction) {
        if (source.isValid())
            m_store->removeAt(source.row());
    } else if (chosen == clearAction) {
        m_store->clearHistory();
    } else if (chosen == unfavoriteAllAction) {
        m_store->clearFavorites();
    }

    hidePreview();   // 删完/清完，光标底下可能已经不是刚才那一条了
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
    // 问**代理**：空不空要按当前选项卡算，历史页有东西不代表收藏页也有
    const bool hasRows = m_proxy->rowCount() > 0;

    m_list->setVisible(hasRows);
    m_empty->setVisible(!hasRows);

    if (!hasRows) {
        m_empty->setText(m_proxy->mode() == ClipFilterProxy::Favorites
                             ? QStringLiteral("还没有收藏。\n\n点每条右边的星星把它收进来。")
                             : QStringLiteral("还没有剪贴板历史。\n\n复制点什么，这里就会出现。"));
    }
}

void MainWindow::onTabChanged(int tabIndex)
{
    m_proxy->setMode(tabIndex == 1 ? ClipFilterProxy::Favorites : ClipFilterProxy::History);

    // 切页必须把这些状态一起清掉，否则会出现「这一页根本没点过的高亮」这种东西：
    //   - 预览：计时器可能正为一条马上要被过滤掉的行计时，到点就会弹一条不在当前页的
    //   - 选中项：新页里恰好在的条目会顶着上一页留下的高亮，还带出左边那条蓝竖条
    //   - 滚动位置：条目变少后旧的像素偏移可能已经越过内容高度
    hidePreview();
    m_previewIndex = QPersistentModelIndex();
    m_previewOnThumb = false;
    m_list->clearSelection();
    m_list->setCurrentIndex(QModelIndex());
    m_list->scrollToTop();

    // invalidateFilter() 到底发 modelReset 还是 layoutChanged 不值得赌，直接自己刷一遍
    updateEmptyState();
}

void MainWindow::toggleFavorite(const QModelIndex &proxyIndex)
{
    const QModelIndex source = m_proxy->mapToSource(proxyIndex);
    if (!source.isValid())
        return;

    const int row = source.row();
    m_store->setFavorite(row, !m_store->isFavorite(row));

    // 在收藏页取消收藏，那一行必须立刻消失；代理不会自己重评估这一行。
    // 不能放在 dataChanged 的槽里做 —— 那是在源信号发出过程中重入。
    m_proxy->refresh();
    updateEmptyState();   // 同上：取消掉最后一条收藏时要切到空状态文案
}

void MainWindow::scheduleHistorySave()
{
    m_saveHistoryTimer->start();
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

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_list->viewport()) {
        switch (event->type()) {
        case QEvent::MouseMove:
            updatePreviewFor(static_cast<QMouseEvent *>(event)->position().toPoint());
            break;
        case QEvent::ToolTip:
            // 把 Qt 自动那个气泡吃掉，改由我们自己计时再弹（延迟见 AppConfig.h）。
            // Qt 那套的延迟（约 0.7 秒）没有公开的调节接口，只能这样接管；
            // **不能让两边都弹** —— 那样同一段文字会出来两次。
            // 用的还是 QToolTip，气泡本身是原生的。
            return true;

        // 点星星不能顺带粘贴，而 QListView::clicked 只带 index **不带坐标**，
        // 没法在它里面区分「点星星」和「点整行」。坐标只有这里拿得到，所以拦在这儿。
        case QEvent::MouseButtonPress: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            const QPoint pos = mouse->position().toPoint();
            const QModelIndex index = m_list->indexAt(pos);
            if (mouse->button() == Qt::LeftButton && index.isValid()
                && starHitRect(m_list->visualRect(index)).contains(pos)) {
                m_starPressed = true;
                m_starIndex = index;   // 持久索引：重排/过滤后自动失效，不会误伤别行
                hidePreview();
                // 吃掉 press：视图就没记下 pressedIndex，抬起时自然不发 clicked
                return true;
            }
            hidePreview();   // 别的按下：就是准备粘贴，先把气泡收掉
            break;
        }

        case QEvent::MouseButtonRelease: {
            if (!m_starPressed)
                break;   // 不是星星手势，放行给视图（正常粘贴走这条路）
            auto *mouse = static_cast<QMouseEvent *>(event);
            const QPoint pos = mouse->position().toPoint();

            // 只有抬起时仍在**同一颗**星星上才算数：
            // 「按下在星上、拖到别处再松开」应该是什么都不发生。
            if (m_starIndex.isValid()
                && starHitRect(m_list->visualRect(m_starIndex)).contains(pos)) {
                toggleFavorite(m_starIndex);
            }
            m_starPressed = false;
            m_starIndex = QPersistentModelIndex();
            hidePreview();
            // **无论如何都吃掉**，这不是求稳而是这里真的会错：在收藏页点星星取消收藏，
            // 那一行当场被过滤掉，抬起时坐标底下已经换成另一行了 ——
            // 漏给视图就会把另一条粘贴出去。
            return true;
        }

        case QEvent::MouseButtonDblClick: {
            // 连着点两下星星时，第二次按下是 DblClick 而不是 Press，不吃就会漏给视图
            auto *mouse = static_cast<QMouseEvent *>(event);
            const QPoint pos = mouse->position().toPoint();
            const QModelIndex index = m_list->indexAt(pos);
            if (index.isValid() && starHitRect(m_list->visualRect(index)).contains(pos))
                return true;
            break;
        }

        case QEvent::Leave:
        case QEvent::Wheel:      // 滚过之后光标底下已经换了一条
            hidePreview();
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MainWindow::updatePreviewFor(const QPoint &viewportPos)
{
    const QModelIndex index = m_list->indexAt(viewportPos);
    const bool valid = index.isValid();
    const bool onThumb = valid && leadingRect(m_list->visualRect(index)).contains(viewportPos);

    // 换了一条（或者进出缩略图那一小块）就把上一条的提示收掉。
    // 比的是持久索引，不是行号 —— 收藏页里取消一次收藏，后面所有条目的行号都会变。
    if (index != m_previewIndex || onThumb != m_previewOnThumb)
        hidePreview();

    m_previewIndex = index;
    m_previewOnThumb = onThumb;

    // 每动一下就重新计时：气泡是「停住不动」之后才出来的，一动就重新等 ——
    // 和系统气泡一个脾气（Qt 自带的那个也会被鼠标移动收掉，不重新计时的话
    // 用户手一抖气泡就再也回不来了）。
    m_previewTimer->stop();
    if (!valid)
        return;

    // index 是代理索引，取数据也走代理（见 pasteRow 上面那段索引语义）
    if (index.data(ClipStore::KindRole).toInt() == int(Clip::Image)) {
        // 图片只在左侧那个缩略图上弹。停在行的别处（比如缩略图右边那片空白）不弹 ——
        // 不然鼠标横扫过一串图片时，屏幕上会一路冒大图。
        if (onThumb)
            m_previewTimer->start(imagePreviewDelayMs());
    } else {
        m_previewTimer->start(textPreviewDelayMs());
    }
}

void MainWindow::showPreview()
{
    // 持久索引在这里自查：计时器启动之后这一行可能已经被删掉、被重排、或者
    // 在收藏页被过滤掉了 —— 那种情况下它就是无效的，直接不弹。
    if (!m_previewIndex.isValid())
        return;

    const QModelIndex index = m_previewIndex;

    if (index.data(ClipStore::KindRole).toInt() == int(Clip::Image)) {
        m_preview->showImage(index.data(ClipStore::ImageRole).value<QImage>());
        // 摆在面板外侧、对着光标的高度。用光标而不是行矩形：面板自己不抢焦点，
        // 光标才是「用户正看着哪儿」的唯一信号。
        m_preview->showNextTo(this, QCursor::pos());
        return;
    }

    // 文本用的是 Qt 原生的气泡（QToolTip），只是由我们决定什么时候弹。
    // 显示什么由模型说了算：Qt::ToolTipRole 给的是全文。
    const QString tip = index.data(Qt::ToolTipRole).toString();
    if (!tip.isEmpty())
        QToolTip::showText(QCursor::pos(), tip, m_list->viewport());
}

void MainWindow::hidePreview()
{
    m_previewTimer->stop();
    m_preview->hide();
    QToolTip::hideText();
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
