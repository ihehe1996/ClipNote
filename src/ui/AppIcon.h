#pragma once

#include <QIcon>

namespace ClipNote {

// 程序图标：resources/clipnote.png，通过 resources/resources.qrc 编进程序。
// 托盘图标和自绘标题栏左上角用的是同一个，所以抽出来共用。
//
// 注意这和 exe 文件本身带的图标**不是一回事**，两个载体缺一不可：
//   - 这个 PNG（qrc + QIcon）管的是运行期画出来的图标：托盘、标题栏、关于对话框
//   - resources/clipnote.rc 编进 PE 资源段的那份，管的是资源管理器在文件上画的：
//     桌面/开始菜单快捷方式、任务栏、卸载项
// 换图标要两处一起换：替换 resources/clipnote.png → 跑一次 tools/make-icon.py
// 重新生成 .ico → 重新构建。只换 png 的话，快捷方式会继续显示 Windows 的默认图标。
QIcon appIcon();

// exe 自己的资源段里有没有图标（就是上面那个 .rc 编进去的）。
// 资源管理器、快捷方式、任务栏认的是它；而缺了它**不会有任何报错** —— 编译、
// 链接、运行全都正常，只是静静地显示成 Windows 的默认图标，所以自检里钉了一条。
bool hasEmbeddedAppIcon();

} // namespace ClipNote
