#include "platform/AutoPaste.h"

#include "AppConfig.h"

#include <QElapsedTimer>
#include <QThread>

#ifdef Q_OS_WIN
#  include <dwmapi.h>

// 老一点的 MinGW 头文件里没有这个常量
#  ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#    define DWMWA_WINDOW_CORNER_PREFERENCE 33
#  endif
#  ifndef DWMWCP_ROUND
#    define DWMWCP_ROUND 2
#  endif
#endif

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace ClipNote {
namespace {

#ifdef Q_OS_WIN

// 等物理按下的修饰键松开。
//
// 用户是用 Alt+Q 唤出面板的，如果他很快去点条目，Alt 在物理上可能还按着，
// 此时合成的 Ctrl+V 到了目标那边就成了 Alt+Ctrl+V —— 错误的命令，
// 某些程序里还会触发它的菜单。超过上限还没松开就干脆不粘。
bool waitForModifiersUp(int timeoutMs)
{
    const int keys[] = { VK_MENU, VK_SHIFT, VK_CONTROL, VK_LWIN, VK_RWIN };

    QElapsedTimer timer;
    timer.start();

    for (;;) {
        bool held = false;
        for (int key : keys) {
            if (GetAsyncKeyState(key) & 0x8000) {
                held = true;
                break;
            }
        }
        if (!held)
            return true;

        if (timer.elapsed() >= timeoutMs)
            return false;

        QThread::msleep(10);
    }
}

bool sendCtrlV()
{
    INPUT inputs[4] = {};

    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'V';

    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = 'V';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;

    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

    // 目标以管理员权限运行时 UIPI 会把注入静默丢掉，返回值是这里唯一的线索
    return SendInput(4, inputs, sizeof(INPUT)) == 4;
}

#endif // Q_OS_WIN

} // namespace

bool pasteIntoFocusedWindow()
{
#ifdef Q_OS_WIN
    if (!waitForModifiersUp(autoPasteModifierTimeoutMs()))
        return false;

    return sendCtrlV();
#else
    return false;
#endif
}

void makeWindowNonActivating(quintptr window)
{
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(window);
    if (!hwnd)
        return;

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_NOACTIVATE)
        return;

    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle | WS_EX_NOACTIVATE);

    // 改完扩展样式要让窗口重算一次边框，样式才真的生效。
    // SWP_NOACTIVATE 很关键 —— 否则这次 SetWindowPos 自己就会把窗口激活。
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
#else
    Q_UNUSED(window)
#endif
}




void setWindowRoundedCorners(quintptr window)
{
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(window);
    if (!hwnd)
        return;

    const DWORD preference = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &preference, sizeof(preference));
#else
    Q_UNUSED(window)
#endif
}

} // namespace ClipNote
