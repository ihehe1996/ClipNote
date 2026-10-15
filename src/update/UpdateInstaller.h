#pragma once

#include <QString>

namespace ClipNote {

// 自更新的「换文件」那一段。
//
// 这里没有按键注入，也不碰别人的窗口，所以和 AutoPaste 不同，自检可以包含它 ——
// stage() 是纯文件操作，自检里能真造一个 zip 进去验。
//
// 真正危险的 launchSwap() 不会被自检调用（它会把本程序替换掉），
// 那份脚本由 tools/test-updater.py 拿桩程序单独验。
namespace UpdateInstaller {

// 一次更新用到的几个路径，都算好存在这里，免得各处重复拼
struct Prepared
{
    QString workDir;    // 本次更新的临时目录，收尾脚本成功后会删掉它
    QString stageDir;   // 新版本的文件堆在这里，收尾脚本把它铺到安装目录
    QString targetExe;  // 要被替换掉的那个 exe，就是本程序自己
};

// 本次更新的临时目录（%TEMP%/ClipNoteUpdate-<版本>）。
// 下载要先落到这里，所以下载之前就得把目录建出来 —— 调用方自己 mkpath。
QString workDirFor(const QString &version);

// 系统目录（System32）下一个工具的绝对路径，找不到就退回名字本身。
//
// **不要**用裸名字去 CreateProcess：PATH 上排在前面的可能是 Git for Windows 或
// Cygwin 带的同名程序 —— 实测 Git 的 tar 是 GNU tar，它解不了 zip ——
// 也可能是个恶意同名文件。自检里造测试 zip 用的是同一个函数，
// 免得测试和生产各自解析出不同的 tar。
QString systemToolPath(const QString &relative);

// 把下载下来的东西铺进暂存目录。
//   .zip → 用系统自带的 tar.exe 解开（Win10 1803 起都有 bsdtar，不引第三方解压库）
//   其它 → 当成本身就是个 exe，直接摆好
// 之后在目录里找出主程序，校验一下它确实是个 PE 文件（开头是 "MZ"），
// 免得把一个 404 页面或者半截文件当成新版本装上去。
bool stage(const QString &downloadedFile, const QString &version,
           Prepared *out, QString *error);

// 安装目录现在能不能写。装在 Program Files 之类的地方就换不了 ——
// 这种事要早说，别等用户点了「立即更新」、下载完了才失败。
bool canReplace(const QString &exePath, QString *error);

// 替换是同步的：写脚本、编码、启动「送葬人」，然后**调用方要立刻退出程序**，
// 否则送葬人等的就是它自己。
bool launchSwap(const Prepared &prepared, QString *error);

// 清掉以前失败留下的临时目录（超过 7 天的）。启动时调一次。
void cleanStaleWorkDirs();

} // namespace UpdateInstaller
} // namespace ClipNote
