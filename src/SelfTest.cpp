#include "SelfTest.h"

#include "AppConfig.h"
#include "model/ClipStore.h"
#include "model/ClipboardWatcher.h"
#include "platform/GlobalHotkey.h"
#include "ui/AppIcon.h"
#include "ui/Tray.h"
#include "update/ReleaseInfo.h"
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
#include <QKeySequence>
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
            store.clear();

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

            // 两个都在时，挑的必须是便携 zip 而不是安装程序
            const QByteArray both = QByteArrayLiteral(R"({
              "tag_name": "v1.0.1",
              "html_url": "https://example.com",
              "assets": [
                { "name": "ClipNote-1.0.1-setup.exe", "size": 3000000,
                  "browser_download_url": "https://example.com/ClipNote-1.0.1-setup.exe" },
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
