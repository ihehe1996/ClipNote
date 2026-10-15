#pragma once

#include <QtGlobal>

namespace ClipNote {

// 往**当前拥有键盘焦点的窗口**注入一次 Ctrl+V。
//
// 面板是不抢焦点的（见 MainWindow.h 的说明），所以焦点从头到尾都在用户原来那个程序上 ——
// 直接发按键就落在正确的地方。不需要记住目标窗口，也不需要把焦点还回去。
//
// 这是全项目唯一会注入输入的文件，单独一个头，好让 SelfTest.cpp 在源头上够不着它。
// **自检绝不能调用这里的东西**：它会往当时拥有焦点的窗口里敲 Ctrl+V，
// 那可能是用户正在编辑的文档。
//
// 返回 false 表示一个键都没发出去，此时内容已经在剪贴板里，用户自己按 Ctrl+V 即可。
bool pasteIntoFocusedWindow();

// 把窗口标记成「永不激活」，也就是 Win32 的 WS_EX_NOACTIVATE。
//
// 这本该是 Qt::WindowDoesNotAcceptFocus 的活，但本机实测它没落到窗口样式里
// （查 exstyle 只有 TOOLWINDOW/TOPMOST/WINDOWEDGE），所以显式再设一次。
//
// 这个性质是硬要求，不是锦上添花：没有它，用户一点面板就会把面板激活，
// 紧接着注入的 Ctrl+V 会打到面板自己身上，而不是用户原来那个程序 ——
// 表现就是「点了没反应」。
void makeWindowNonActivating(quintptr window);

// 请系统把窗口画成圆角（Windows 11 的能力）。
//
// 面板是无边框的，DWM 默认不给它圆角，四个直角在浅色背景上很扎眼。
// 老系统上这个属性不被识别，调用会失败 —— 静默忽略，直角就直角。
void setWindowRoundedCorners(quintptr window);

} // namespace ClipNote
