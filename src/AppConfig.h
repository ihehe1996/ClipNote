#pragma once

#include <QSize>
#include <QString>
#include <QStringList>

namespace ClipNote {

// 当前版本号，形如 "v1.0.0"。
//
// **不要在代码里硬编码它** —— 它是 CMakeLists.txt 里 project(... VERSION ...)
// 生成的编译期宏。发新版只改那一行，这个值和 exe 里的版本、以及 Release 的 tag
// 三者天然一致。
//
// 这里的兜底只是为了「万一没过 CMake」时不至于编不过；真出现 v0.0.0-unknown
// 说明构建配置被绕过去了，更新器会拿它去比版本，那是查不出更新的。
#ifndef CLIPNOTE_VERSION
#  define CLIPNOTE_VERSION "v0.0.0-unknown"
#endif

inline QString appVersion() { return QString::fromLatin1(CLIPNOTE_VERSION); }

// 默认全局热键。主程序注册的是它，--selftest 校验的也是它 —— 改这一处，两边一起变。
//
// 选热键前的注意事项：
//
// 1. 热键是系统级独占的。要换组合先跑一次 --selftest，日志末尾会列出几个候选在本机
//    是空闲还是被占；本机实测 Ctrl+Alt+C 已被别的程序占用（RegisterHotKey 返回
//    ERROR_HOTKEY_ALREADY_REGISTERED）。
//
// 2. Alt+单个字母 的副作用比组合键大：它是全局的，会把别的程序里对应的菜单助记键
//    一并抢走。挑它就意味着接受这点。
//
// 3. Qt 解析快捷键时 Windows 键写作 "Meta"，不是 "Win"。
//    写成 "Win+Shift+C" 会得到一个空序列，注册必然失败。
inline QString defaultHotkey() { return QStringLiteral("Alt+Q"); }

// 自动粘贴总开关。关掉之后，选中一条历史只会写回剪贴板，
// 不做任何合成按键 —— 用户自己按 Ctrl+V。
inline bool autoPasteEnabled() { return true; }

// 面板的默认尺寸。只在「没存过尺寸」或者「存的值不合法」时用 —— 正常情况下面板
// 会沿用用户上次拖出来的大小（记在 QSettings 的 panel/size 里）。
inline QSize defaultPanelSize() { return QSize(360, 440); }

// 面板能拖到多小。低于这个尺寸排版就开始坏了，两个方向各有各的下限：
//
//   宽度 —— 标题栏在约 170px 以下会把标题挤没、齿轮和关闭两颗按钮互压；
//           列表每一行的正文可用宽度 = 行宽 − 83 − 相对时间的宽度（约 40~55px）。
//   高度 —— 标题栏 32 + body 上下边距 12，剩下至少要放得下两行（每行 48）。
inline QSize minimumPanelSize() { return QSize(260, 200); }

// 等修饰键（Alt 等）物理抬起的最长时间，毫秒。
//
// 用 Alt+Q 唤出面板后马上按回车时，Alt 可能还按着，此时注入会变成 Alt+Ctrl+V。
// 超过这个时间还没松开就放弃注入，退化成「内容已在剪贴板，你自己按 Ctrl+V」。
inline int autoPasteModifierTimeoutMs() { return 600; }

// 开机自启时用的开关：启动后只驻留托盘，**不弹出面板**。
//
// 安装程序写进 HKCU\...\Run 的就是 `"ClipNote.exe" --silent`。
// 没有它的话每次开机都会在屏幕中间弹一个剪贴板面板出来 —— 那是打扰，不是功能。
// 用户手点图标或按热键照样能唤出。
inline bool isSilentStartRequested(const QStringList &arguments)
{
    return arguments.contains(QStringLiteral("--silent"));
}

// ---- 检查更新 ----

// 更新源：GitHub 上的「用户名/仓库名」。
//
// **这里是空的，所以「检查更新」现在会明确地告诉你还没配好**，而不是假装查过了。
// 建好仓库、发过一次 Release 之后，把这一行填掉即可 —— 更新走的是
// https://api.github.com/repos/{它}/releases/latest，不需要自己搭服务器。
//
// 发 Release 时的约定（两条，都影响能不能升级成功）：
//   1. tag 写成版本号，v 前缀随便（"v0.2.0" 和 "0.2.0" 都认）
//   2. 挂一个附件：优先 .zip（可以整包带上依赖的 DLL），其次是单个 .exe
//      建议发版的 exe 用 windeployqt 打成 zip，否则用裸 exe 只能换主程序，
//      带不动新增的 DLL —— 比如这个版本刚开始用 Qt6Network.dll，
//      老用户目录里就没有它。
inline QString updateRepo() { return QStringLiteral("ihehe1996/ClipNote"); }

// 启动之后隔多久去悄悄查一次，毫秒。
// 等一会儿是为了别和启动时的托盘、热键、面板抢时间，也不至于让冷启动更慢。
inline int updateStartupDelayMs() { return 5000; }

// 启动时的检查**没有**节流，每次启动都查一次。
// 这里本来有个「距上次检查不到 N 小时就跳过」，去掉了：本程序是托盘常驻 + 开机自启，
// 一次「启动」基本就是一次开机，频率本来就低；留着它只会让「刚重启完反而查不到更新」
// 变成常态。真要省请求，省下的量一天也就几次，不值当。

} // namespace ClipNote
