#!/usr/bin/env python
"""检查剪贴板面板的核心性质（不需要注入任何键盘输入）。

  1. 面板窗口带 WS_EX_NOACTIVATE —— 点它不会把面板激活
  2. 挂上了 owner —— 因而不会在任务栏里冒出按钮
  3. 最小化/最大化按钮是关掉的 —— 面板没有任务栏按钮，一旦能最小化就再也找不回来
  4. 唤出面板前后，前台窗口没有变化 —— 用户原来那个程序连光标都不会闪
  5. 四条边和四个角返回缩放命中码，正中间是 HTCLIENT —— 窗口能拖边缘改大小

第 5 条问的是 WM_NCHITTEST：在边缘返回 HTLEFT/HTBOTTOM 这类码，系统才会给缩放光标、
才谈得上拖动；而正中间必须仍然是 HTCLIENT，否则整扇窗口都成了非客户区，列表点不动。

第 1、4 条是「单击即粘贴」能工作的前提：没有它们，用户一点面板就把面板激活了，
紧接着注入的 Ctrl+V 会打到面板自己身上，表现就是「点了没反应」。

这几条都容易悄悄回归，而且都不是写一行代码就能看出来的：

  - `Qt::WindowDoesNotAcceptFocus` 在本机**不会**真的把 WS_EX_NOACTIVATE 落到窗口样式上，
    得手工补（见 src/platform/AutoPaste.cpp）。
  - owner 不能手工设 GWLP_HWNDPARENT —— Qt 在 show() 时会把它冲掉，必须走 Qt 的
    transient parent（即 setParent）。

唤出面板走的是给窗口 PostMessage 一个 WM_HOTKEY，命中程序自己的热键处理路径。
刻意不用 SendInput —— 那会把按键敲进用户当前聚焦的窗口，可能污染他正在编辑的文档。

用法：
    python tools/check-panel.py [ClipNote.exe 路径]
退出码 0 = 全部通过。
"""
import ctypes
import subprocess
import sys
import time
from ctypes import wintypes

DEFAULT_EXE = r"build\debug\ClipNote.exe"

GWL_STYLE = -16
GWL_EXSTYLE = -20
WS_EX_NOACTIVATE = 0x08000000
WS_EX_TOPMOST = 0x00000008
WS_MINIMIZEBOX = 0x00020000
WS_MAXIMIZEBOX = 0x00010000
GW_OWNER = 4
WM_HOTKEY = 0x0312
WM_LBUTTONDOWN, WM_LBUTTONUP = 0x0201, 0x0202
WM_NCHITTEST = 0x0084

# 命中码。面板在边缘必须返回这些，系统才会把光标画成缩放箭头；
# 我们借此确认「拖边缘能改大小」这件事在原生那一层是通的。
HTCLIENT = 1
HTLEFT, HTRIGHT, HTTOP, HTBOTTOM = 10, 11, 12, 15
HTTOPLEFT, HTTOPRIGHT, HTBOTTOMLEFT, HTBOTTOMRIGHT = 13, 14, 16, 17
RESIZE_HITS = {HTLEFT, HTRIGHT, HTTOP, HTBOTTOM,
               HTTOPLEFT, HTTOPRIGHT, HTBOTTOMLEFT, HTBOTTOMRIGHT}

HOTKEY_ID = 0x434E            # 对应 src/platform/GlobalHotkey.cpp 里的 kHotkeyId（'CN'）
PANEL_TITLE = "ClipNote"      # 面板的标题；隐藏 owner 用的是另一个标题，好区分
PANEL_WIDTH = 360             # 面板的逻辑宽度，用来把图钉按钮的位置换算出来

# 必须声明 DPI 感知：否则拿到的窗口/客户区尺寸是被系统虚拟化过的（125% 缩放下会小 1/4），
# 据此算出来的点击坐标（消息里是**物理**像素）就会偏，点到别的地方去。
ctypes.windll.user32.SetProcessDPIAware()

user32 = ctypes.WinDLL("user32", use_last_error=True)
k32 = ctypes.WinDLL("kernel32", use_last_error=True)

user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetWindowLongW.argtypes = [wintypes.HWND, ctypes.c_int]
user32.GetWindowLongW.restype = ctypes.c_long
user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.c_void_p]
user32.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
user32.GetWindow.restype = wintypes.HWND
user32.GetForegroundWindow.restype = wintypes.HWND
user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.c_void_p]
user32.DefWindowProcW.argtypes = [wintypes.HWND, wintypes.UINT,
                                  ctypes.c_size_t, ctypes.c_ssize_t]
user32.DefWindowProcW.restype = ctypes.c_ssize_t
user32.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR,
                                   wintypes.DWORD, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_int, wintypes.HWND,
                                   wintypes.HMENU, wintypes.HINSTANCE, wintypes.LPVOID]
user32.CreateWindowExW.restype = wintypes.HWND
user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
                                ctypes.c_int, ctypes.c_int, wintypes.UINT]
k32.GetModuleHandleW.restype = wintypes.HMODULE
k32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
# WM_NCHITTEST 要拿返回值，所以必须用 SendMessageW（PostMessage 拿不到）。
user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT,
                                wintypes.WPARAM, wintypes.LPARAM]
user32.SendMessageW.restype = ctypes.c_ssize_t

EnumProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


def window_class(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, buf, 256)
    return buf.value


def window_title(hwnd):
    buf = ctypes.create_unicode_buffer(512)
    user32.GetWindowTextW(hwnd, buf, 512)
    return buf.value


def style(hwnd):
    return user32.GetWindowLongW(hwnd, GWL_STYLE) & 0xFFFFFFFF


def ex_style(hwnd):
    return user32.GetWindowLongW(hwnd, GWL_EXSTYLE) & 0xFFFFFFFF


def window_rect(hwnd):
    rect = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    return rect


def hit_test(hwnd, x, y):
    """问窗口「这个**屏幕**坐标压在你身上的哪一部分」。

    WM_NCHITTEST 的 lParam 是屏幕坐标（不是客户区坐标），DPI 感知上面已经声明过，
    所以这里的物理像素和 GetWindowRect 给的是同一个坐标系。
    """
    return user32.SendMessageW(hwnd, WM_NCHITTEST, 0,
                               ((y & 0xFFFF) << 16) | (x & 0xFFFF))


def foreground_pid():
    hwnd = user32.GetForegroundWindow()
    if not hwnd:
        return 0
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return pid.value


def top_level_windows(pid):
    found = []

    def callback(hwnd, _lparam):
        p = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid:
            found.append(hwnd)
        return True

    user32.EnumWindows(EnumProc(callback), 0)
    return found


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_EXE

    proc = subprocess.Popen([exe])
    print(f"ClipNote pid = {proc.pid}")
    time.sleep(3)

    panel = None
    for hwnd in top_level_windows(proc.pid):
        if window_class(hwnd).startswith("Qt") and window_title(hwnd) == PANEL_TITLE:
            panel = hwnd

    if panel is None:
        print(f"!! 没找到标题为「{PANEL_TITLE}」的面板窗口")
        proc.terminate()
        return 1

    # 「前台没被抢走」这条比较的是按下热键前后的前台进程 —— 而用户就在同一台机器上干活，
    # 他在这两秒里切一下窗口就会让它误报。所以重试几次，任何一次干净通过就算过。
    focus_kept = False
    before_pid = after_pid = foreground_pid()
    for _attempt in range(3):
        # 面板启动时就显示，而 WM_HOTKEY 走的是 toggle —— 先把它收起来，
        # 否则下面那次「唤出」实际是「收起」。
        if user32.IsWindowVisible(panel):
            user32.PostMessageW(panel, WM_HOTKEY, HOTKEY_ID, 0)
            time.sleep(1.0)

        before_pid = foreground_pid()
        user32.PostMessageW(panel, WM_HOTKEY, HOTKEY_ID, 0)
        time.sleep(1.2)
        after_pid = foreground_pid()

        if before_pid == after_pid and before_pid != proc.pid:
            focus_kept = True
            break

    visible = bool(user32.IsWindowVisible(panel))
    ex = ex_style(panel)
    st = style(panel)
    noactivate = bool(ex & WS_EX_NOACTIVATE)
    has_owner = bool(user32.GetWindow(panel, GW_OWNER))
    no_min = not (st & WS_MINIMIZEBOX)
    no_max = not (st & WS_MAXIMIZEBOX)

    print(f"\n面板 hwnd=0x{panel:X} class={window_class(panel)}")
    print(f"  exstyle=0x{ex:08X}   style=0x{st:08X}")
    print(f"  可见={visible}  owner=0x{user32.GetWindow(panel, GW_OWNER):X}")
    print(f"  前台 pid: {before_pid} -> {after_pid}")
    print()

    # 面板能不能拖边缘改大小，由「窗口在边缘返回什么命中码」决定。
    # 逐点问一遍：四条边的中点、四个角、以及正中间（正中间必须是 HTCLIENT，
    # 否则整扇窗口都会变成非客户区，列表就点不动了）。
    rect = window_rect(panel)
    mid_x = (rect.left + rect.right) // 2
    mid_y = (rect.top + rect.bottom) // 2
    probes = [
        ("左边", HTLEFT, rect.left + 1, mid_y),
        ("右边", HTRIGHT, rect.right - 1, mid_y),
        ("上边", HTTOP, mid_x, rect.top + 1),
        ("下边", HTBOTTOM, mid_x, rect.bottom - 1),
        ("左上角", HTTOPLEFT, rect.left + 1, rect.top + 1),
        ("右上角", HTTOPRIGHT, rect.right - 1, rect.top + 1),
        ("左下角", HTBOTTOMLEFT, rect.left + 1, rect.bottom - 1),
        ("右下角", HTBOTTOMRIGHT, rect.right - 1, rect.bottom - 1),
    ]

    print(f"  窗口矩形=({rect.left},{rect.top})-({rect.right},{rect.bottom})")
    edge_ok = True
    for name, want, x, y in probes:
        got = hit_test(panel, x, y)
        if got != want:
            edge_ok = False
        print(f"  {name}: 期望 {want}，实际 {got}")
    center_hit = hit_test(panel, mid_x, mid_y)
    print(f"  正中间: 期望 {HTCLIENT}，实际 {center_hit}")
    print()

    results = [
        (visible, "面板被唤出（可见）", None),
        (noactivate, "带 WS_EX_NOACTIVATE（点它不会激活面板）", None),
        (has_owner, "挂了 owner（不会在任务栏冒出按钮）", None),
        (no_min and no_max, "最小化/最大化已关（避免最小化后再也找不回来）", None),
        (focus_kept, f"前台窗口没被抢走（pid 保持 {before_pid}）", None),
        (edge_ok, "四条边和四个角返回缩放命中码（能拖边缘改大小）", None),
        (center_hit == HTCLIENT,
         f"正中间返回 HTCLIENT（列表照常收鼠标），实际 {center_hit}", None),
    ]
    for ok, desc, _detail in results:
        print(f"[{'OK ' if ok else 'FAIL'}] {desc}")

    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()

    return 0 if all(ok for ok, _desc, _detail in results) else 1


if __name__ == "__main__":
    sys.exit(main())
