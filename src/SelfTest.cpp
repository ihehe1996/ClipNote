#include "SelfTest.h"

#include "AppConfig.h"
#include "model/ClipFilterProxy.h"
#include "model/ClipHistoryFile.h"
#include "model/ClipImageFile.h"
#include "model/ClipPageProxy.h"
#include "model/ClipStore.h"
#include "model/ClipboardWatcher.h"
#include "platform/GlobalHotkey.h"
#include "ui/AppIcon.h"
#include "ui/Tray.h"
#include "update/ReleaseInfo.h"
#include "update/UpdateChecker.h"
#include "update/UpdateInstaller.h"
#include "update/Version.h"

// 刻意不包含 platform/AutoPaste.h：那个头里是会向焦点窗口注入按键的
// pasteIntoFocusedWindow()。够不着它，就绝不会在自检里误伤用户正在编辑的窗口。
// 这条边界不要破坏，可以用符号表验证：
//   nm -C <SelfTest.cpp.obj> | grep -iE "pasteInto|SendInput|SetForegroundWindow"

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QPixmap>
#include <QProcess>
#include <QTextStream>
#include <QWidget>

#include <utility>

namespace ClipNote {
namespace {

// 换热键时的备选，自检里会逐个报空闲还是被占
const QStringList &candidateHotkeys()
{
    static const QStringList list = {
        QStringLiteral("Ctrl+Alt+N"),  QStringLiteral("Ctrl+Alt+C"),
        QStringLiteral("Ctrl+Alt+X"),  QStringLiteral("Ctrl+Alt+F9"),
        QStringLiteral("Ctrl+Shift+C"), QStringLiteral("Alt+Shift+C"),
    };
    return list;
}

} // namespace

bool isSelfTestRequest(const QStringList &arguments)
{
    return arguments.contains(QStringLiteral("--selftest"));
}

int runSelfTest(const QStringList &arguments)
{
    Q_UNUSED(arguments)

    QFile file(QDir::current().filePath(QStringLiteral("selftest.log")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return 2;

    QTextStream out(&file);
    out << QStringLiteral("ClipNote 自检  ")
        << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) << '\n';
    out << QStringLiteral("Qt ") << QString::fromLatin1(qVersion()) << '\n';
    out << QStringLiteral("----------------------------------------\n");

    int failed = 0;
    const auto check = [&out, &failed](const QString &name, bool ok, const QString &detail) {
        out << (ok ? QStringLiteral("[ OK ] ") : QStringLiteral("[FAIL] ")) << name;
        if (!detail.isEmpty())
            out << QStringLiteral("  —— ") << detail;
        out << '\n';
        if (!ok)
            ++failed;
    };
    // 仅供参考，不计入成败
    const auto note = [&out](const QString &name, const QString &detail) {
        out << QStringLiteral("[ -- ] ") << name << QStringLiteral("  —— ") << detail << '\n';
    };

    // 1. 平台插件：不是 windows 的话后面几项都无从谈起
    const QString platform = QGuiApplication::platformName();
    check(QStringLiteral("Qt 平台插件"), platform == QStringLiteral("windows"),
          QStringLiteral("platformName = %1").arg(platform));

    // 2. 托盘
    const bool trayOk = Tray::isAvailable();
    check(QStringLiteral("系统托盘可用"), trayOk,
          trayOk ? QStringLiteral("可以常驻")
                 : QStringLiteral("托盘不可用，程序会改成直接显示面板"));

    // 2b. 图标资源。资源路径写错了不会编译报错，只会静悄悄地变成一个空图标
    //     （托盘和标题栏都会跟着空掉），所以值得钉一条。
    {
        const QIcon icon = appIcon();
        const QPixmap pixmap = icon.pixmap(16, 16);
        check(QStringLiteral("程序图标能从资源里读出来"),
              !icon.isNull() && !pixmap.isNull(),
              QStringLiteral("内置尺寸 %1 种，取 16px 位图%2")
                  .arg(icon.availableSizes().size())
                  .arg(pixmap.isNull() ? QStringLiteral("为空") : QStringLiteral("正常")));
    }

    // 2c. exe **自己的资源段**里有没有图标（resources/clipnote.rc 编进去的那份）。
    //     和上面 2b 查的不是一回事：2b 查的是 qrc 里的 PNG，管的是运行期画出来的
    //     图标（托盘、标题栏）；这条查的是资源管理器在文件上画的图标 ——
    //     桌面/开始菜单快捷方式、任务栏、卸载项全都靠它。
    //     缺了它编译链接运行全都正常，只是静静地显示成 Windows 的默认图标
    //     （装完之后快捷方式没有 logo，就是这个症状），所以值得钉一条。
    check(QStringLiteral("exe 资源段里带着程序图标"), hasEmbeddedAppIcon(),
          QStringLiteral("LoadImage 取资源 ID 1"));

    // 3. 剪贴板：写个哨兵值再读回来，读完还原用户原来的内容
    if (QClipboard *clipboard = QGuiApplication::clipboard()) {
        const QString saved = clipboard->text();
        const QString sentinel =
            QStringLiteral("ClipNote selftest %1").arg(QDateTime::currentMSecsSinceEpoch());

        clipboard->setText(sentinel);
        const QString readBack = clipboard->text();
        clipboard->setText(saved); // 别把用户原来的剪贴板内容弄丢

        check(QStringLiteral("剪贴板读写"), readBack == sentinel,
              readBack == sentinel ? QStringLiteral("写入后能读回，原有内容已还原")
                                   : QStringLiteral("写进去 %1，读回来 %2").arg(sentinel, readBack));
    } else {
        check(QStringLiteral("剪贴板读写"), false, QStringLiteral("QGuiApplication::clipboard() 返回空"));
    }

    // 4. 全局热键：注册主程序真正要用的那个组合。要个 HWND，所以建个不显示的窗口当宿主。
    //    热键是系统级独占的，ClipNote 本体正开着时这里必然失败 —— 属正常现象。
    {
        QWidget probe;
        GlobalHotkey hotkey;

        const QKeySequence shortcut(defaultHotkey());
        const QString text = shortcut.toString(QKeySequence::PortableText);

        if (hotkey.grab(probe.winId(), shortcut))
            check(QStringLiteral("默认全局热键"), true, QStringLiteral("%1 注册成功并已注销").arg(text));
        else
            check(QStringLiteral("默认全局热键"), false, hotkey.errorString());

        hotkey.release();

        // 5. 候选组合的占用情况
        out << QStringLiteral("----------------------------------------\n");
        out << QStringLiteral("候选热键在本机的占用情况（要换热键就看这里）\n");
        for (const QString &candidate : candidateHotkeys()) {
            if (hotkey.grab(probe.winId(), QKeySequence(candidate)))
                note(candidate, QStringLiteral("空闲"));
            else
                note(candidate, hotkey.errorString());
            hotkey.release();
        }
    }

    // 6. 剪贴板历史的存储逻辑。ClipStore 是纯数据、刻意不碰 QClipboard，
    //    所以这里能直接构造出来灌数据验证 —— 这是整套自检里最实在的一段。
    {
        ClipStore store;
        int changes = 0;
        const auto onChange = [&changes]() { ++changes; };
        QObject::connect(&store, &QAbstractItemModel::rowsInserted, &store,
                         [&onChange](const QModelIndex &, int, int) { onChange(); });
        QObject::connect(&store, &QAbstractItemModel::rowsRemoved, &store,
                         [&onChange](const QModelIndex &, int, int) { onChange(); });

        store.pushText(QStringLiteral("第一条"));
        store.pushText(QStringLiteral("第二条"));
        check(QStringLiteral("历史：最新的排在最前"),
              store.rowCount() == 2 && store.textAt(0) == QStringLiteral("第二条"),
              QStringLiteral("共 %1 条，第 0 条是「%2」").arg(store.rowCount()).arg(store.textAt(0)));

        // 重复推送当前最新项：应当完全无感。程序重新发布同样的剪贴板内容时走这条路，
        // 要是每次都挪一下，用户正翻列表时选中项会被无谓重置。
        const int before = changes;
        store.pushText(QStringLiteral("第二条"));
        check(QStringLiteral("历史：重复推送最新项不发信号"),
              changes == before && store.rowCount() == 2,
              QStringLiteral("信号数 %1 → %2，条数 %3")
                  .arg(before).arg(changes).arg(store.rowCount()));

        // 重复一条更靠下的：挪到最前，而不是变成 3 条
        store.pushText(QStringLiteral("第一条"));
        check(QStringLiteral("历史：重复项挪到最前而不是新增一条"),
              store.rowCount() == 2 && store.textAt(0) == QStringLiteral("第一条"),
              QStringLiteral("共 %1 条，第 0 条是「%2」").arg(store.rowCount()).arg(store.textAt(0)));

        store.pushText(QStringLiteral("   \n\t  "));
        check(QStringLiteral("历史：纯空白内容被忽略"),
              store.rowCount() == 2, QStringLiteral("共 %1 条").arg(store.rowCount()));

        check(QStringLiteral("历史：越界取值返回空串而不是崩"),
              store.textAt(-1).isEmpty() && store.textAt(999).isEmpty(),
              QStringLiteral("textAt(-1)=「%1」 textAt(999)=「%2」")
                  .arg(store.textAt(-1), store.textAt(999)));

        // 上限：推超量的不同内容，条数应被截断且最旧的被挤掉
        ClipStore capped;
        const int pushed = ClipStore::kMaxItems + 20;
        for (int i = 0; i < pushed; ++i)
            capped.pushText(QStringLiteral("clip-%1").arg(i));

        const QString newest = QStringLiteral("clip-%1").arg(pushed - 1);
        check(QStringLiteral("历史：超过上限后从最旧的开始丢"),
              capped.rowCount() == ClipStore::kMaxItems && capped.textAt(0) == newest
                  && capped.textAt(ClipStore::kMaxItems - 1) == QStringLiteral("clip-20"),
              QStringLiteral("推了 %1 条剩 %2 条，第 0 条是「%3」，最后一条是「%4」")
                  .arg(pushed).arg(capped.rowCount())
                  .arg(capped.textAt(0), capped.textAt(ClipStore::kMaxItems - 1)));

        // 删除
        ClipStore editable;
        editable.pushText(QStringLiteral("A"));
        editable.pushText(QStringLiteral("B"));
        editable.pushText(QStringLiteral("C")); // 列表变成 C, B, A
        editable.removeAt(1);                   // 删中间的 B

        check(QStringLiteral("历史：删掉中间一条，其余顺序不变"),
              editable.rowCount() == 2 && editable.textAt(0) == QStringLiteral("C")
                  && editable.textAt(1) == QStringLiteral("A"),
              QStringLiteral("共 %1 条：「%2」「%3」")
                  .arg(editable.rowCount()).arg(editable.textAt(0), editable.textAt(1)));

        // 图片：ClipStore 同样不碰剪贴板，所以这里也能直接灌图验证
        ClipStore pics;
        QImage red(40, 30, QImage::Format_ARGB32);
        red.fill(Qt::red);
        QImage blue(60, 20, QImage::Format_ARGB32);
        blue.fill(Qt::blue);

        pics.pushImage(red);
        pics.pushImage(blue);

        const QString pic0 = pics.data(pics.index(0, 0), Qt::DisplayRole).toString();
        const QVariant thumb = pics.data(pics.index(0, 0), ClipStore::ThumbRole);
        check(QStringLiteral("历史：图片能收进来，带缩略图和尺寸标签"),
              pics.rowCount() == 2
                  && pics.data(pics.index(0, 0), ClipStore::KindRole).toInt() == int(Clip::Image)
                  && !thumb.value<QPixmap>().isNull()
                  && pic0 == QStringLiteral("图片  60 × 20"),
              QStringLiteral("共 %1 条，第 0 条显示为「%2」，缩略图%3")
                  .arg(pics.rowCount()).arg(pic0)
                  .arg(thumb.value<QPixmap>().isNull() ? QStringLiteral("为空") : QStringLiteral("有")));

        pics.pushImage(red); // 再复制同一张
        check(QStringLiteral("历史：重复的图片挪到最前而不是新增一条"),
              pics.rowCount() == 2
                  && pics.data(pics.index(0, 0), Qt::DisplayRole).toString()
                         == QStringLiteral("图片  40 × 30"),
              QStringLiteral("共 %1 条，第 0 条显示为「%2」")
                  .arg(pics.rowCount())
                  .arg(pics.data(pics.index(0, 0), Qt::DisplayRole).toString()));

        int removeSignals = 0;
        QObject::connect(&editable, &QAbstractItemModel::rowsRemoved, &editable,
                         [&removeSignals](const QModelIndex &, int, int) { ++removeSignals; });
        editable.removeAt(-1);
        editable.removeAt(99);
        check(QStringLiteral("历史：越界删除是空操作"),
              editable.rowCount() == 2 && removeSignals == 0,
              QStringLiteral("共 %1 条，发了 %2 次信号").arg(editable.rowCount()).arg(removeSignals));
    }

    // 6b. 收藏。收藏项有两条特殊待遇（不受上限影响、不被「清空历史」清掉），
    //     而且有两个很容易写漏的地方：重复推送会把收藏冲掉、以及过滤代理的索引映射。
    {
        ClipStore store;
        store.pushText(QStringLiteral("A"));
        store.pushText(QStringLiteral("B"));
        store.pushText(QStringLiteral("C"));   // 列表是 C, B, A

        store.setFavorite(1, true);            // 收藏 B
        check(QStringLiteral("收藏：标记能设上，也能读回来"),
              store.isFavorite(1) && !store.isFavorite(0) && !store.isFavorite(2)
                  && store.data(store.index(1, 0), ClipStore::FavoriteRole).toBool(),
              QStringLiteral("第 1 条收藏=%1").arg(store.isFavorite(1)));

        // 这个坑很隐蔽：pushText 发现重复内容时是**新建**一个 Clip 再置顶的，
        // 不把旧的收藏标记抄过去，「收藏过的内容再复制一次」收藏就悄悄没了。
        store.pushText(QStringLiteral("B"));
        check(QStringLiteral("收藏：重复复制不会把收藏标记冲掉"),
              store.isFavorite(0) && store.textAt(0) == QStringLiteral("B"),
              QStringLiteral("B 又推了一次，置顶后收藏=%1").arg(store.isFavorite(0)));

        // 上限只管非收藏的：灌到超过 kMaxItems 之后收藏的一条都不能少
        ClipStore capped;
        capped.pushText(QStringLiteral("收藏的"));
        capped.setFavorite(0, true);
        for (int i = 0; i < ClipStore::kMaxItems + 20; ++i)
            capped.pushText(QStringLiteral("clip-%1").arg(i));

        // 收藏那条是最早推的，被一次次置顶挤到最旧的末尾；截断只丢非收藏的，
        // 所以它最后应该稳稳待在最后一行。总条数 = 上限 + 收藏的 1 条。
        const int favoriteRow = capped.rowCount() - 1;
        check(QStringLiteral("收藏：不受 200 条上限影响，被挤到最旧的末尾也留着"),
              capped.rowCount() == ClipStore::kMaxItems + 1
                  && capped.textAt(favoriteRow) == QStringLiteral("收藏的")
                  && capped.isFavorite(favoriteRow),
              QStringLiteral("共 %1 条（上限 %2），最后一条是「%3」收藏=%4")
                  .arg(capped.rowCount()).arg(ClipStore::kMaxItems)
                  .arg(capped.textAt(favoriteRow)).arg(capped.isFavorite(favoriteRow)));

        // 「清空历史（保留收藏）」只清非收藏的
        ClipStore mixed;
        mixed.pushText(QStringLiteral("普通一"));
        mixed.pushText(QStringLiteral("收藏的"));
        mixed.setFavorite(0, true);            // 现在是 收藏的, 普通一
        mixed.pushText(QStringLiteral("普通二"));
        mixed.clearHistory();
        check(QStringLiteral("收藏：清空历史保留收藏"),
              mixed.rowCount() == 1 && mixed.textAt(0) == QStringLiteral("收藏的"),
              QStringLiteral("清完剩 %1 条：「%2」").arg(mixed.rowCount()).arg(mixed.textAt(0)));

        // 「取消全部收藏」只取消标记，不删条目
        const int beforeUnfavorite = mixed.rowCount();
        mixed.clearFavorites();
        check(QStringLiteral("收藏：取消全部收藏只是取消标记，不删条目"),
              mixed.rowCount() == beforeUnfavorite && !mixed.isFavorite(0),
              QStringLiteral("条数 %1 → %2，第 0 条收藏=%3")
                  .arg(beforeUnfavorite).arg(mixed.rowCount()).arg(mixed.isFavorite(0)));
    }

    // 6c. 过滤代理的索引映射。**这是整套里最要紧的一条**：视图挂的是代理，
    //     把代理索引当成源行号用不会崩、不会报错 —— 历史页行号恰好重合看着还正常，
    //     一切到收藏页就静默取错条目。这里把映射关系钉死。
    {
        ClipStore store;
        store.pushText(QStringLiteral("第0条"));
        store.pushText(QStringLiteral("第1条"));
        store.pushText(QStringLiteral("第2条"));
        store.pushText(QStringLiteral("第3条"));   // 列表：3, 2, 1, 0

        store.setFavorite(3, true);                // 收藏「第0条」（在最底下）
        store.setFavorite(1, true);                // 收藏「第2条」

        ClipFilterProxy proxy;
        proxy.setSourceModel(&store);

        const int allRows = proxy.rowCount();
        proxy.setMode(ClipFilterProxy::Favorites);
        const int favRows = proxy.rowCount();

        const QModelIndex first = proxy.index(0, 0);
        const QModelIndex second = proxy.index(1, 0);
        const int firstSource = proxy.mapToSource(first).row();
        const int secondSource = proxy.mapToSource(second).row();

        check(QStringLiteral("收藏页：只剩收藏的，且顺序和源一致"),
              allRows == 4 && favRows == 2 && firstSource == 1 && secondSource == 3,
              QStringLiteral("历史页 %1 条，收藏页 %2 条，映射到源的第 %3、%4 条")
                  .arg(allRows).arg(favRows).arg(firstSource).arg(secondSource));

        // 关键断言：**通过代理取数据**必须拿到那一行自己的内容。
        // 要是代码里写成 m_store->data(代理索引)，这里就会取到源里第 0 条。
        const QString viaProxy = proxy.data(first, ClipStore::TextRole).toString();
        const QString viaSource = store.textAt(firstSource);
        check(QStringLiteral("收藏页：经代理取到的正是那一行的内容（不是按代理行号去源里瞎取）"),
              viaProxy == viaSource && viaProxy == QStringLiteral("第2条"),
              QStringLiteral("代理取到「%1」，源的第 %2 条是「%3」")
                  .arg(viaProxy).arg(firstSource).arg(viaSource));

        // 取消收藏之后重新过滤，那一行应当从收藏页消失
        store.setFavorite(firstSource, false);
        proxy.refresh();
        check(QStringLiteral("收藏页：取消收藏后刷新，那一行立刻消失"),
              proxy.rowCount() == 1 && proxy.mapToSource(proxy.index(0, 0)).row() == 3,
              QStringLiteral("刷新后剩 %1 条").arg(proxy.rowCount()));
    }

    // 6d. 历史落盘。走真实文件（写在 %TEMP% 下一个自己的目录里，用完删掉），
    //     验的是「写出去再读回来，内容/顺序/收藏/时间戳都对得上」。
    {
        ClipStore original;
        original.pushText(QStringLiteral("落盘甲\n第二行"));
        original.pushText(QStringLiteral("落盘乙"));
        original.setFavorite(1, true);

        const QString dir = QDir(QDir::temp().filePath(QStringLiteral("ClipNote-selftest-history")))
                                .absolutePath();
        QDir().mkpath(dir);
        const QString path = QDir(dir).filePath(QStringLiteral("history.json"));

        QString error;
        const bool saved = saveHistory(path, original, &error);
        ClipStore restored;
        const bool loaded = saved && loadHistory(path, &restored, &error);

        const bool same = loaded && restored.rowCount() == original.rowCount()
                          && restored.textAt(0) == QStringLiteral("落盘乙")
                          && restored.textAt(1) == QStringLiteral("落盘甲\n第二行")
                          && restored.isFavorite(1)   // 收藏的是第 1 条（老的「落盘甲」）
                          && restored.data(restored.index(1, 0), ClipStore::CapturedAtRole)
                                 .toDateTime()
                                 .toSecsSinceEpoch()
                                 == original.data(original.index(1, 0), ClipStore::CapturedAtRole)
                                        .toDateTime()
                                        .toSecsSinceEpoch();

        check(QStringLiteral("落盘：写出去再读回来，内容/顺序/收藏/时间戳都对得上"),
              same,
              loaded ? QStringLiteral("共 %1 条，第 0 条「%2」收藏=%3")
                           .arg(restored.rowCount()).arg(restored.textAt(0)).arg(restored.isFavorite(0))
                     : QStringLiteral("失败：%1").arg(error));

        // 图片：像素落到 images/<hash>.png，JSON 里只记元数据。
        // ClipStore 是 QObject，不能拷贝构造，所以重新灌一个。
        const QString imagesDir = QDir(dir).filePath(QStringLiteral("images"));
        ClipStore withPic;
        withPic.setImagesDir(imagesDir);
        QImage pic(20, 20, QImage::Format_ARGB32);
        pic.fill(Qt::green);
        pic.setPixel(3, 4, qRgb(1, 2, 3));   // 点一个特殊像素，"读回来是不是同一张"靠它
        withPic.pushImage(pic);
        withPic.pushText(QStringLiteral("落盘甲"));   // 文本推在后面 → 图片被挤到第 1 条
        saveHistory(path, withPic, &error);

        ClipStore reloaded;
        reloaded.setImagesDir(imagesDir);
        loadHistory(path, &reloaded, &error);

        QFile dump(path);
        dump.open(QIODevice::ReadOnly);
        const QByteArray raw = dump.readAll();
        dump.close();

        const uint picHash = withPic.items().at(1).imageHash;
        const bool fileWritten = QFile::exists(imagePathFor(imagesDir, picHash))
                                 && QFile::exists(thumbPathFor(imagesDir, picHash));

        check(QStringLiteral("落盘：图片的像素写到 images/ 下的文件里，JSON 里只记元数据"),
              reloaded.rowCount() == 2 && fileWritten && !raw.contains("base64")
                  && raw.contains(QString::number(picHash, 16).toLatin1()),
              QStringLiteral("源 %1 条 → 读回 %2 条，图片文件%3，JSON %4 字节")
                  .arg(withPic.rowCount()).arg(reloaded.rowCount())
                  .arg(fileWritten ? QStringLiteral("在") : QStringLiteral("不在")).arg(raw.size()));

        const QModelIndex imgIndex = reloaded.index(1, 0);
        check(QStringLiteral("落盘：图片条目读回来带缩略图和尺寸"),
              reloaded.data(imgIndex, ClipStore::KindRole).toInt() == int(Clip::Image)
                  && !reloaded.data(imgIndex, ClipStore::ThumbRole).value<QPixmap>().isNull()
                  && reloaded.items().at(1).imageSize == QSize(20, 20),
              QStringLiteral("显示为「%1」")
                  .arg(reloaded.data(imgIndex, Qt::DisplayRole).toString()));

        // 懒加载的核心：读回来的时候原图**不在内存里**，要的时候才去磁盘读
        const bool notInMemory =
            reloaded.data(imgIndex, ClipStore::ImageRole).value<QImage>().isNull();
        const QImage lazilyLoaded = reloaded.loadFullImage(1);
        check(QStringLiteral("懒加载：原图不常驻内存，要的时候能读出来且和写进去的一致"),
              notInMemory && !lazilyLoaded.isNull() && lazilyLoaded.size() == QSize(20, 20)
                  && lazilyLoaded.pixel(3, 4) == qRgb(1, 2, 3),
              QStringLiteral("内存里%1，磁盘读回 %2×%3")
                  .arg(notInMemory ? QStringLiteral("没有") : QStringLiteral("有"))
                  .arg(lazilyLoaded.width()).arg(lazilyLoaded.height()));

        // 原图文件没了 → 读不到，但条目还在（降级不崩）。
        // **必须换一个 store 实例**：上面那次 loadFullImage 已经把图缓存起来了，
        // 同一个实例再问会拿到缓存（那本身是想要的行为 —— 刚看过就还在内存里），
        // 验不出「文件没了会怎样」。
        QFile::remove(imagePathFor(imagesDir, picHash));
        ClipStore secondRead;
        secondRead.setImagesDir(imagesDir);
        loadHistory(path, &secondRead, &error);
        check(QStringLiteral("懒加载：原图文件被删掉之后读不到，但条目还在、不崩"),
              secondRead.loadFullImage(1).isNull() && secondRead.rowCount() == 2,
              QStringLiteral("读回空=%1，条数 %2")
                  .arg(secondRead.loadFullImage(1).isNull()).arg(secondRead.rowCount()));

        // 缩略图也没了的条目，读历史时应当**直接跳过**（宁可少一条，也不留点不开的坏条目）
        QFile::remove(thumbPathFor(imagesDir, picHash));
        ClipStore skipped;
        skipped.setImagesDir(imagesDir);
        loadHistory(path, &skipped, &error);
        check(QStringLiteral("落盘：图片文件不在的条目读历史时被跳过，只留文本"),
              skipped.rowCount() == 1 && skipped.textAt(0) == QStringLiteral("落盘甲"),
              QStringLiteral("读回 %1 条：「%2」").arg(skipped.rowCount()).arg(skipped.textAt(0)));

        // 删条目要把文件一起删掉，不然磁盘只涨不落
        ClipStore victim;
        victim.setImagesDir(imagesDir);
        victim.pushImage(pic);
        const uint victimHash = victim.items().at(0).imageHash;
        victim.removeAt(0);
        check(QStringLiteral("落盘：删掉条目会把它的图片文件一起删掉"),
              !QFile::exists(imagePathFor(imagesDir, victimHash))
                  && !QFile::exists(thumbPathFor(imagesDir, victimHash)),
              QStringLiteral("哈希 %1 的文件还在吗：%2")
                  .arg(victimHash).arg(QFile::exists(imagePathFor(imagesDir, victimHash))));

        // 文件不在 → 当空历史，不报错、不崩
        ClipStore fresh;
        const bool missing = loadHistory(QDir(dir).filePath(QStringLiteral("没有这个文件.json")),
                                         &fresh, &error);
        check(QStringLiteral("落盘：历史文件不存在时当空历史启动"),
              !missing && fresh.rowCount() == 0,
              QStringLiteral("返回 %1，条数 %2").arg(missing).arg(fresh.rowCount()));

        QDir(dir).removeRecursively();
    }

    // 6e. 分页。纯模型层，不碰 UI —— 这块的信号规则最容易写错，所以逐条钉死。
    {
        ClipStore store;
        for (int i = 0; i < 55; ++i)
            store.pushText(QStringLiteral("clip-%1").arg(i));   // 列表：clip-54 … clip-0

        ClipFilterProxy filter;
        filter.setSourceModel(&store);
        ClipPageProxy page;
        page.setSourceModel(&filter);

        check(QStringLiteral("分页：只显示一页，但底层仍是全量"),
              page.rowCount() == itemsPerPage() && store.rowCount() == 55 && page.hasMore(),
              QStringLiteral("显示 %1 行，底层 %2 条，还有更多=%3")
                  .arg(page.rowCount()).arg(store.rowCount()).arg(page.hasMore()));

        check(QStringLiteral("分页：显示的是最新的那几条"),
              page.data(page.index(0, 0), ClipStore::TextRole).toString() == QStringLiteral("clip-54"),
              QStringLiteral("第 0 行是「%1」")
                  .arg(page.data(page.index(0, 0), ClipStore::TextRole).toString()));

        // loadMore 是**追加在尾部**：视图行数变多，已有的行位置不动
        const QString row19Before = page.data(page.index(19, 0), ClipStore::TextRole).toString();
        page.loadMore();
        check(QStringLiteral("分页：再加载一页是追加，前面的行不动"),
              page.rowCount() == 2 * itemsPerPage()
                  && page.data(page.index(19, 0), ClipStore::TextRole).toString() == row19Before
                  && page.data(page.index(20, 0), ClipStore::TextRole).toString()
                         == QStringLiteral("clip-34"),
              QStringLiteral("显示 %1 行，第 20 行是「%2」")
                  .arg(page.rowCount())
                  .arg(page.data(page.index(20, 0), ClipStore::TextRole).toString()));

        // 最要紧的一条：**已经满页时来了新条目**，行数不能变（发 dataChanged，不发 rowsInserted）
        const QString row18Before = page.data(page.index(18, 0), ClipStore::TextRole).toString();
        int insertedSignals = 0;
        QObject::connect(&page, &QAbstractItemModel::rowsInserted, &page,
                         [&insertedSignals](const QModelIndex &, int, int) { ++insertedSignals; });
        store.pushText(QStringLiteral("刚刚复制的"));
        check(QStringLiteral("分页：满页时来了新条目，行数不变、内容整体下移一格"),
              page.rowCount() == 2 * itemsPerPage()
                  && insertedSignals == 0
                  && page.data(page.index(0, 0), ClipStore::TextRole).toString()
                         == QStringLiteral("刚刚复制的")
                  && page.data(page.index(19, 0), ClipStore::TextRole).toString() == row18Before,
              QStringLiteral("行数 %1，发了 %2 次 rowsInserted，第 19 行是「%3」（应为「%4」）")
                  .arg(page.rowCount()).arg(insertedSignals)
                  .arg(page.data(page.index(19, 0), ClipStore::TextRole).toString(), row18Before));

        // 没满页时来新条目，那才是真的多一行
        ClipStore small;
        for (int i = 0; i < 5; ++i)
            small.pushText(QStringLiteral("s-%1").arg(i));
        ClipFilterProxy smallFilter;
        smallFilter.setSourceModel(&small);
        ClipPageProxy smallPage;
        smallPage.setSourceModel(&smallFilter);
        int smallInserted = 0;
        QObject::connect(&smallPage, &QAbstractItemModel::rowsInserted, &smallPage,
                         [&smallInserted](const QModelIndex &, int, int) { ++smallInserted; });
        const int beforeSmall = smallPage.rowCount();
        small.pushText(QStringLiteral("s-新"));
        check(QStringLiteral("分页：没满页时来新条目，行数加一"),
              beforeSmall == 5 && smallPage.rowCount() == 6 && smallInserted == 1,
              QStringLiteral("行数 %1 → %2，发了 %3 次 rowsInserted")
                  .arg(beforeSmall).arg(smallPage.rowCount()).arg(smallInserted));

        // 分页必须作用在**过滤之后**：收藏页里显示的得是前 20 个收藏，
        // 而不是"前 20 条里的收藏"（那样早期收藏的条目会永远看不见）
        ClipFilterProxy favFilter;
        favFilter.setSourceModel(&store);
        ClipPageProxy favPage;
        favPage.setSourceModel(&favFilter);
        // 条数要**多于一页**，否则验不出"分页作用在过滤之后"（一共才十几条的话
        // 一页就装下了，分不分页看不出区别）
        for (int row = 34; row < store.rowCount(); ++row)   // 靠后的 22 条
            store.setFavorite(row, true);
        favFilter.setMode(ClipFilterProxy::Favorites);
        favPage.resetPaging();

        const QString firstFavorite = store.textAt(34);
        check(QStringLiteral("分页：作用在过滤之后（收藏页显示的是前 20 个收藏）"),
              favPage.rowCount() == itemsPerPage() && favPage.hasMore()
                  && favPage.data(favPage.index(0, 0), ClipStore::TextRole).toString()
                         == firstFavorite,
              QStringLiteral("收藏页显示 %1 行，第 0 行是「%2」（源第 34 条是「%3」）")
                  .arg(favPage.rowCount())
                  .arg(favPage.data(favPage.index(0, 0), ClipStore::TextRole).toString(),
                       firstFavorite));

        // 切页复位成一页
        favPage.loadMore();
        const int expanded = favPage.rowCount();
        favPage.resetPaging();
        check(QStringLiteral("分页：切选项卡后页数拨回一页"),
              expanded > itemsPerPage() && favPage.rowCount() == itemsPerPage(),
              QStringLiteral("%1 行 → 复位成 %2 行").arg(expanded).arg(favPage.rowCount()));

        // 落盘**不受分页影响** —— 这是唯一会静默丢数据的做法（重启后历史只剩一页）
        const QString pageDir =
            QDir(QDir::temp().filePath(QStringLiteral("ClipNote-selftest-paging"))).absolutePath();
        QDir().mkpath(pageDir);
        const QString pagePath = QDir(pageDir).filePath(QStringLiteral("history.json"));
        QString pageError;
        saveHistory(pagePath, store, &pageError);
        QFile pageDump(pagePath);
        pageDump.open(QIODevice::ReadOnly);
        const QJsonDocument pageDoc = QJsonDocument::fromJson(pageDump.readAll());
        pageDump.close();
        const int savedCount = pageDoc.object().value(QStringLiteral("items")).toArray().size();
        check(QStringLiteral("分页：落盘写的是全量，不是当前显示的那一页"),
              savedCount == store.rowCount() && favPage.rowCount() == itemsPerPage()
                  && savedCount > favPage.rowCount(),
              QStringLiteral("存了 %1 条，底层 %2 条，收藏页当前只显示 %3 行")
                  .arg(savedCount).arg(store.rowCount()).arg(favPage.rowCount()));
        QDir(pageDir).removeRecursively();

        // 信号审计：把过滤代理切模式时**实际**发的信号记进日志。
        // 分页层的不少设计是在躲 invalidateFilter() 的行为（它到底发 rowsInserted
        // 还是 layoutChanged 是版本相关的、没保证），与其赌，不如记下来 ——
        // 以后 Qt 升级改了行为，日志里立刻看得见，而不是靠 UI 偶发跳动去猜。
        {
            // 要有"过滤真的改变了行数"的情形，不然代理可能优化成什么都不发
            // —— 这本身也是个结论：**不能指望切模式一定发信号**，所以 MainWindow
            // 切页后是显式再叫一次分页层，而不是等信号。
            ClipStore auditStore;
            auditStore.pushText(QStringLiteral("甲"));
            auditStore.pushText(QStringLiteral("乙"));
            auditStore.pushText(QStringLiteral("丙"));
            auditStore.setFavorite(1, true);
            ClipFilterProxy audit;
            audit.setSourceModel(&auditStore);

            QStringList emitted;
            // 用**带完整参数**的签名接：带个"少参数"的 lambda 理论上也行，但这里
            // 是在做信号审计，仪器本身不能有疑问
            QObject::connect(&audit, &QAbstractItemModel::modelReset, &audit,
                             [&emitted] { emitted << QStringLiteral("modelReset"); });
            QObject::connect(&audit, &QAbstractItemModel::layoutChanged, &audit,
                             [&emitted](const QList<QPersistentModelIndex> &,
                                        QAbstractItemModel::LayoutChangeHint) {
                                 emitted << QStringLiteral("layoutChanged");
                             });
            QObject::connect(&audit, &QAbstractItemModel::rowsInserted, &audit,
                             [&emitted](const QModelIndex &, int, int) {
                                 emitted << QStringLiteral("rowsInserted");
                             });
            QObject::connect(&audit, &QAbstractItemModel::rowsRemoved, &audit,
                             [&emitted](const QModelIndex &, int, int) {
                                 emitted << QStringLiteral("rowsRemoved");
                             });
            QObject::connect(&audit, &QAbstractItemModel::dataChanged, &audit,
                             [&emitted](const QModelIndex &, const QModelIndex &, const QList<int> &) {
                                 emitted << QStringLiteral("dataChanged");
                             });

            const int before = audit.rowCount();
            audit.setMode(ClipFilterProxy::Favorites);
            audit.refresh();
            const int after = audit.rowCount();
            note(QStringLiteral("过滤代理 setMode/refresh 实际发的信号（分页层要能接住）"),
                 QStringLiteral("行数 %1 → %2，发的信号：%3")
                     .arg(before).arg(after)
                     .arg(emitted.isEmpty() ? QStringLiteral("（什么都没发）")
                                            : emitted.join(QStringLiteral(" + "))));
        }
    }

    // 7. 「点条目写回剪贴板」不该让历史重排。
    //    这条要走真实剪贴板（dataChanged 是同步发的，不用转事件循环），
    //    所以放在上面剪贴板往返检查之后，写完照样还原用户原来的内容。
    {
        ClipStore store;
        ClipboardWatcher watcher(&store);
        QClipboard *clipboard = QGuiApplication::clipboard();

        if (clipboard) {
            const QString saved = clipboard->text();

            // watcher 构造时会把当前剪贴板内容先收进来，清掉才好在确定的起点上断言
            //（这里没有收藏项，clearHistory 等价于清空）
            store.clearHistory();

            store.pushText(QStringLiteral("第一条"));
            store.pushText(QStringLiteral("第二条"));
            const int before = store.rowCount();
            const QString picked = QStringLiteral("第一条");

            // 模拟用户点了列表里第 1 条：面板会先告知 watcher，再写剪贴板
            watcher.suppressChange(picked);
            clipboard->setText(picked);

            check(QStringLiteral("历史：点条目写回剪贴板不会让列表重排"),
                  store.rowCount() == before && store.textAt(0) == QStringLiteral("第二条")
                      && store.textAt(1) == QStringLiteral("第一条"),
                  QStringLiteral("共 %1 条，第 0 条是「%2」")
                      .arg(store.rowCount()).arg(store.textAt(0)));

            // 反过来：用户自己复制同样的内容，应当照常挪到最前面（抑制只作用于我们写的那一次）
            store.pushText(picked);
            check(QStringLiteral("历史：抑制只生效一次，用户自己再复制仍会置顶"),
                  store.rowCount() == before && store.textAt(0) == picked,
                  QStringLiteral("共 %1 条，第 0 条是「%2」")
                      .arg(store.rowCount()).arg(store.textAt(0)));

            clipboard->setText(saved);
        }
    }

    // 8. 版本号解析与比较。更新功能里唯一一块纯逻辑，也最容易悄悄写错 ——
    //    版本比错的后果是「有新版却不说」或者「天天喊你更新同一个版本」。
    {
        const auto v = [](const char *text) {
            Version parsed;
            const bool ok = parseVersion(QString::fromLatin1(text), &parsed);
            return std::make_pair(ok, parsed.toString());
        };

        // case 1: 剥掉 v 前缀，补齐段数
        const auto withV = v("v1.2.3");
        const auto noV = v("1.2.3");
        const auto shortForm = v("1.2");
        check(QStringLiteral("版本：v 前缀被剥掉，缺的段按 0 补"),
              withV.first && noV.first && shortForm.first && withV.second == QStringLiteral("1.2.3")
                  && noV.second == QStringLiteral("1.2.3")
                  && shortForm.second == QStringLiteral("1.2.0"),
              QStringLiteral("v1.2.3→%1  1.2.3→%2  1.2→%3")
                  .arg(withV.second, noV.second, shortForm.second));

        // 真实会遇到的那种：四段式 tag。显示时第四段不能被吞掉 ——
        // 显示成 "8.9.8" 会让人以为更新的还是自己在用的那一版。
        Version fourPart, threePart;
        const bool fourOk = parseVersion(QStringLiteral("8.9.8.1"), &fourPart);
        const bool threeOk = parseVersion(QStringLiteral("8.9.8"), &threePart);
        check(QStringLiteral("版本：四段式 tag 也认，且比三段式新"),
              fourOk && threeOk && fourPart.toString() == QStringLiteral("8.9.8.1")
                  && threePart.toString() == QStringLiteral("8.9.8")
                  && isNewerVersion(fourPart, threePart),
              QStringLiteral("8.9.8.1 显示为 %1，8.9.8 显示为 %2")
                  .arg(fourPart.toString(), threePart.toString()));

        // 预发布后缀不参与比较，这是文档里写明的取舍
        Version rc, plain;
        const bool rcOk = parseVersion(QStringLiteral("0.2.0-rc1"), &rc);
        const bool plainOk = parseVersion(QStringLiteral("0.2.0"), &plain);
        check(QStringLiteral("版本：预发布后缀不影响比较（已知取舍）"),
              rcOk && plainOk && compareVersions(rc, plain) == 0,
              QStringLiteral("0.2.0-rc1 与 0.2.0 判为同一版本"));

        // 宁可拒绝，也不要瞎比：解析不出来就必须返回 false
        Version junk;
        check(QStringLiteral("版本：解析不出来的串被拒绝"),
              !parseVersion(QStringLiteral("latest"), &junk)
                  && !parseVersion(QStringLiteral(""), &junk)
                  && !parseVersion(QStringLiteral("v"), &junk),
              QStringLiteral("latest / 空串 / 单个 v 都返回 false"));

        // 相等不算「有更新」—— 否则每次查都提示同一个版本
        check(QStringLiteral("版本：相等不算有更新"),
              !isNewerVersion(plain, plain)
                  && isNewerVersion(plain, Version{{0, 1, 0, 0}})
                  && !isNewerVersion(Version{{0, 1, 0, 0}}, plain),
              QStringLiteral("0.2.0 vs 0.2.0 → 无更新；0.2.0 vs 0.1.0 → 有更新"));

        // 编进来的那个版本号本身。它要是格式不对（或者没从 CMake 传进来、
        // 落到兜底值上），更新器会拿一个垃圾去比 —— 表现是「永远查不出更新」，
        // 而界面上一点异常都看不出来。发新版改错了 CMakeLists 就该在这里拦下来。
        Version compiled;
        check(QStringLiteral("版本：编译进来的版本号合法，没落到兜底值"),
              parseVersion(appVersion(), &compiled)
                  && !appVersion().contains(QStringLiteral("unknown")),
              QStringLiteral("appVersion() = %1（来自 CMakeLists 的 project VERSION）")
                  .arg(appVersion()));
    }

    // 9. 解析 GitHub 的 release 响应。用的是**真实抓下来的响应**精简过的样本
    //    （只砍了字段和附件数量，字段名和结构原样保留），所以它同时也在验
    //    「字段名写得对不对」—— 这是手写 mock 抓不到的那类错。
    {
        const QByteArray payload = QByteArrayLiteral(R"({
          "html_url": "https://github.com/example/ClipNote/releases/tag/v8.9.8.1",
          "tag_name": "v8.9.8.1",
          "name": "ClipNote release 8.9.8.1",
          "draft": false,
          "prerelease": false,
          "published_at": "2026-09-24T15:21:27Z",
          "message": "ignored: 有 tag_name 时不该被当成错误",
          "assets": [
            { "name": "checksums.sha256", "content_type": "application/octet-stream",
              "size": 1271, "digest": "sha256:78bc138EFF0675589b7887ed3114d30acdcd807f19f36b3ba4095b44173d1e36",
              "browser_download_url": "https://example.com/v8.9.8.1/checksums.sha256" },
            { "name": "ClipNote-8.9.8.1.exe", "content_type": "application/x-msdownload",
              "size": 6632744, "digest": "sha256:a53c5116b55ac93fedd7808e71096badecbf10a55f2be9b28b741591b8917a73",
              "browser_download_url": "https://example.com/v8.9.8.1/ClipNote-8.9.8.1.exe" },
            { "name": "ClipNote-8.9.8.1.zip", "content_type": "application/x-zip-compressed",
              "size": 8090883,
              "browser_download_url": "https://example.com/v8.9.8.1/ClipNote-8.9.8.1.zip" }
          ]
        })");

        ReleaseInfo info;
        QString parseError;
        const bool parsed = parseGitHubRelease(payload, &info, &parseError);

        check(QStringLiteral("更新：能解析 GitHub 的 release 响应"),
              parsed && info.tagName == QStringLiteral("v8.9.8.1") && info.assets.size() == 3,
              parsed ? QStringLiteral("tag=%1，附件 %2 个，发布页 %3")
                           .arg(info.tagName)
                           .arg(info.assets.size())
                           .arg(info.pageUrl.isEmpty() ? QStringLiteral("缺") : QStringLiteral("有"))
                     : parseError);

        const ReleaseAsset *picked = parsed ? pickUpdateAsset(info) : nullptr;
        check(QStringLiteral("更新：优先挑 .zip（它才能带上新增的依赖 DLL）"),
              picked && picked->name.endsWith(QStringLiteral(".zip")),
              picked ? QStringLiteral("挑中了 %1").arg(picked->name)
                     : QStringLiteral("一个都没挑中"));

        // 发安装包之后 Release 里会同时挂着便携 zip 和安装程序 exe。
        // 安装程序也是 .exe，一旦被挑中，stage() 会把它当成「新版本的主程序」
        // 拿去替换用户手上的 ClipNote.exe —— 用户下次双击弹出的就是安装界面。
        // 这条钉的就是「宁可不更新，也不能挑错」。
        {
            const QByteArray installerOnly = QByteArrayLiteral(R"({
              "tag_name": "v1.0.1",
              "html_url": "https://example.com",
              "assets": [
                { "name": "ClipNote-1.0.1-setup.exe", "size": 3000000,
                  "browser_download_url": "https://example.com/ClipNote-1.0.1-setup.exe" }
              ]
            })");

            ReleaseInfo onlyInstaller;
            QString ignoredError;
            const bool ok = parseGitHubRelease(installerOnly, &onlyInstaller, &ignoredError);
            const ReleaseAsset *pickedInstaller =
                ok ? pickUpdateAsset(onlyInstaller) : nullptr;

            check(QStringLiteral("更新：只有安装程序时不挑它（挑错会把主程序换成安装程序）"),
                  ok && pickedInstaller == nullptr,
                  ok ? (pickedInstaller ? QStringLiteral("居然挑中了 %1").arg(pickedInstaller->name)
                                        : QStringLiteral("正确跳过，宁可不更新"))
                     : ignoredError);

            // 两个都在时，挑的必须是便携 zip 而不是安装程序。
            // 这条用的是现在的后缀 -install，上面那条用的是改名前的 -setup ——
            // 两个拼写老 Release 里都有，都得跳过
            const QByteArray both = QByteArrayLiteral(R"({
              "tag_name": "v1.0.1",
              "html_url": "https://example.com",
              "assets": [
                { "name": "ClipNote-1.0.1-install.exe", "size": 3000000,
                  "browser_download_url": "https://example.com/ClipNote-1.0.1-install.exe" },
                { "name": "ClipNote-1.0.1.zip", "size": 8000000,
                  "browser_download_url": "https://example.com/ClipNote-1.0.1.zip" }
              ]
            })");

            ReleaseInfo bothInfo;
            const bool bothOk = parseGitHubRelease(both, &bothInfo, &ignoredError);
            const ReleaseAsset *pickedBoth = bothOk ? pickUpdateAsset(bothInfo) : nullptr;

            check(QStringLiteral("更新：便携 zip 和安装程序同时在时，挑 zip"),
                  bothOk && pickedBoth
                      && pickedBoth->name == QStringLiteral("ClipNote-1.0.1.zip"),
                  pickedBoth ? QStringLiteral("挑中了 %1").arg(pickedBoth->name)
                             : QStringLiteral("一个都没挑中"));
        }

        // digest 是 "sha256:xxx" 形式的，冒号前缀要去掉、统一成小写
        const ReleaseAsset *first = parsed && !info.assets.isEmpty() ? &info.assets.first() : nullptr;
        check(QStringLiteral("更新：digest 里的 sha256 前缀被剥掉并统一成小写"),
              first && first->sha256 == QStringLiteral(
                           "78bc138eff0675589b7887ed3114d30acdcd807f19f36b3ba4095b44173d1e36"),
              first ? (first->sha256.isEmpty() ? QStringLiteral("没解析出来")
                                               : first->sha256.left(16) + QStringLiteral("…"))
                    : QStringLiteral("没有附件"));

        // 仓库不存在时 GitHub 回的是 {"message":"Not Found"}，照样是 200/404 的 JSON 体，
        // 不显式挡一下就会解析出一个 tag 为空串的「假 release」
        ReleaseInfo ignored;
        QString notFoundError;
        check(QStringLiteral("更新：仓库不存在的响应被当成错误，而不是「没有新版本」"),
              !parseGitHubRelease(QByteArrayLiteral(R"({"message":"Not Found"})"),
                                  &ignored, &notFoundError)
                  && notFoundError.contains(QStringLiteral("Not Found")),
              notFoundError);

        check(QStringLiteral("更新：非 JSON 响应被拒绝"),
              !parseGitHubRelease(QByteArrayLiteral("<html>502 Bad Gateway</html>"),
                                  &ignored, &notFoundError),
              notFoundError);

        // GitHub 对「仓库不存在」和「仓库存在但没发过 Release」都回 404。
        // 这两件事用户该干的完全不同（去改配置 / 去发个 Release），
        // 所以提示必须分得开 —— 早先那条「找不到这个仓库或它还没有发布过 Release」
        // 把两个原因糊在一句里，等于什么都没说。
        {
            const QString repo = QStringLiteral("someone/ClipNote");
            const QString missing = repoNotFoundMessage(repo);
            const QString noRelease = noReleaseYetMessage(repo);

            check(QStringLiteral("更新：404 的两种情况给出不同的提示"),
                  !missing.isEmpty() && !noRelease.isEmpty() && missing != noRelease
                      && missing.contains(repo) && noRelease.contains(repo)
                      && missing.contains(QStringLiteral("updateRepo"))
                      && noRelease.contains(QStringLiteral("Release")),
                  QStringLiteral("两条提示内容不同，都点明了仓库名，也各自指出了该做什么"));
        }
    }

    // 10. 自更新换文件之前的「铺暂存目录」那一步。这是纯文件操作，
    //     不需要联网也不需要动真正的 exe，所以能在自检里真跑一遍。
    {
        const QString sandbox =
            QDir::temp().filePath(QStringLiteral("ClipNote-selftest-stage"));
        QDir(sandbox).removeRecursively();
        QDir().mkpath(sandbox);

        // 造两个假文件：真的 MZ 头不必写，校验只看开头两个字节
        const auto writeFake = [](const QString &path, const QByteArray &head) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
                return false;
            file.write(head);
            file.write(QByteArray(64, '\0'));
            return true;
        };

        // 下载下来的文件必须落在 workDirFor(版本) 里面 —— **和真的更新流程一模一样**。
        //
        // 这一点不是讲究，是踩过：早先这里图省事把测试文件放在沙箱根目录，
        // 而 stage() 早期版本会把整个工作目录清空一遍（连刚下载的文件一起），
        // 测试照样全绿，真用起来却必然失败。测的摆放方式一旦和产品不一样，
        // 这种错就永远测不出来。
        const auto downloadTo = [](const QString &version, const QString &name,
                                   const QByteArray &head) {
            const QString dir = UpdateInstaller::workDirFor(version);
            QDir().mkpath(dir);
            const QString path = QDir(dir).filePath(name);
            QFile file(path);
            if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                file.write(head);
                file.write(QByteArray(64, '\0'));
            }
            return path;
        };

        // --- 分支一：下载到的是个裸 exe ---
        const QString bareExe =
            downloadTo(QStringLiteral("9.9.9-bare"), QStringLiteral("ClipNote.exe"),
                       QByteArrayLiteral("MZ"));

        UpdateInstaller::Prepared prepared;
        QString stageError;
        const bool bareOk = UpdateInstaller::stage(bareExe, QStringLiteral("9.9.9-bare"),
                                                   &prepared, &stageError);
        check(QStringLiteral("更新：裸 exe 能被铺进暂存目录并通过 PE 校验"),
              bareOk && QFileInfo::exists(QDir(prepared.stageDir).filePath(QStringLiteral("ClipNote.exe"))),
              bareOk ? QStringLiteral("暂存目录 %1").arg(QDir::toNativeSeparators(prepared.stageDir))
                     : stageError);

        // --- 分支二：下载到的是个 zip（用产品同一套 tar 解析来打包）---
        const QString zipSrc = QDir(sandbox).filePath(QStringLiteral("zipsrc"));
        QDir().mkpath(zipSrc);
        writeFake(QDir(zipSrc).filePath(QStringLiteral("ClipNote.exe")), QByteArrayLiteral("MZ"));
        writeFake(QDir(zipSrc).filePath(QStringLiteral("Qt6Network.dll")), QByteArrayLiteral("MZ"));

        const QString zipPath =
            downloadTo(QStringLiteral("9.9.9-zip"), QStringLiteral("ClipNote-9.9.9.zip"),
                       QByteArray());
        QProcess tar;
        tar.setProgram(UpdateInstaller::systemToolPath(QStringLiteral("tar.exe")));
        tar.setWorkingDirectory(zipSrc);
        tar.setArguments({QStringLiteral("-a"), QStringLiteral("-cf"),
                          QFileInfo(zipPath).absoluteFilePath(),
                          QStringLiteral("ClipNote.exe"), QStringLiteral("Qt6Network.dll")});
        tar.start();
        const bool tarOk = tar.waitForFinished(60000) && tar.exitCode() == 0;

        UpdateInstaller::Prepared zipPrepared;
        const bool zipOk = tarOk
                           && UpdateInstaller::stage(zipPath, QStringLiteral("9.9.9-zip"),
                                                     &zipPrepared, &stageError);
        check(QStringLiteral("更新：zip 能解开并定位到主程序（顺带验证 tar 可用）"),
              zipOk && QFileInfo::exists(QDir(zipPrepared.stageDir).filePath(QStringLiteral("ClipNote.exe")))
                  && QFileInfo::exists(QDir(zipPrepared.stageDir).filePath(QStringLiteral("Qt6Network.dll"))),
              zipOk ? QStringLiteral("主程序和依赖 DLL 都在 %1")
                          .arg(QDir::toNativeSeparators(zipPrepared.stageDir))
                    : (tarOk ? stageError : QStringLiteral("打包测试 zip 就失败了")));

        // --- 反面：一个不是可执行文件的「exe」必须被挡下来 ---
        // 挡的是「Release 里挂错了附件」或者「下到了一个 HTML 错误页」
        const QString htmlFile =
            downloadTo(QStringLiteral("9.9.9-bad"), QStringLiteral("ClipNote.exe"),
                       QByteArrayLiteral("<html>"));

        UpdateInstaller::Prepared badPrepared;
        const bool badRejected = !UpdateInstaller::stage(htmlFile, QStringLiteral("9.9.9-bad"),
                                                        &badPrepared, &stageError);
        check(QStringLiteral("更新：开头不是 MZ 的文件被拒绝，不会拿去替换程序"),
              badRejected, badRejected ? QStringLiteral("已拒绝") : QStringLiteral("居然通过了"));

        // --- 反面：zip 里没有主程序 ---
        const QString emptyZip =
            downloadTo(QStringLiteral("9.9.9-noexe"), QStringLiteral("ClipNote-empty.zip"),
                       QByteArray());
        QProcess tar2;
        tar2.setProgram(UpdateInstaller::systemToolPath(QStringLiteral("tar.exe")));
        tar2.setWorkingDirectory(zipSrc);
        tar2.setArguments({QStringLiteral("-a"), QStringLiteral("-cf"),
                           QFileInfo(emptyZip).absoluteFilePath(),
                           QStringLiteral("Qt6Network.dll")});
        tar2.start();
        tar2.waitForFinished(60000);

        UpdateInstaller::Prepared noExePrepared;
        const bool noExeRejected = !UpdateInstaller::stage(emptyZip, QStringLiteral("9.9.9-noexe"),
                                                          &noExePrepared, &stageError);
        check(QStringLiteral("更新：包里没有主程序时报错，不瞎猜一个 exe 顶上"),
              noExeRejected, noExeRejected ? QStringLiteral("已拒绝") : QStringLiteral("居然通过了"));

        // 自检造出来的工作目录要自己收干净，别在用户的 %TEMP% 里留东西
        for (const char *version : {"9.9.9-bare", "9.9.9-zip", "9.9.9-bad", "9.9.9-noexe"})
            QDir(UpdateInstaller::workDirFor(QString::fromLatin1(version))).removeRecursively();
        QDir(sandbox).removeRecursively();
    }

    // 11. 自更新的换文件那一步（launchSwap）**刻意不在自检里跑**：
    //     它会把本程序替换掉，而自检正跑在这个程序里。
    //     那份 PowerShell 脚本由 tools/test-updater.py 拿两个桩程序单独验，
    //     验的是同一份 resources/apply-update.ps1。
    note(QStringLiteral("自更新的换文件"),
         QStringLiteral("会把本程序替换掉，自检里不能跑；脚本由 tools/test-updater.py 单独验"));

    // 12. 自动粘贴。刻意只留一行说明，不做任何检查：
    //    它要往当时拥有焦点的窗口里注入 Ctrl+V，跑一次就可能把内容敲进用户正在编辑的
    //    文档。这个「没测」要写在日志里，而不是静默略过。
    note(QStringLiteral("自动粘贴"),
         QStringLiteral("会向当前焦点窗口注入 Ctrl+V，自检里绝不调用，只能在真实使用中手动验证"));

    out << QStringLiteral("----------------------------------------\n");
    out << (failed == 0 ? QStringLiteral("全部通过\n")
                        : QStringLiteral("%1 项未通过\n").arg(failed));

    return failed == 0 ? 0 : 1;
}

} // namespace ClipNote
