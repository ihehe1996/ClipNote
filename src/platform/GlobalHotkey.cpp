#include "platform/GlobalHotkey.h"

#include <QCoreApplication>
#include <QKeyCombination>
#include <QKeySequence>

#ifdef Q_OS_WIN
// MinGW 的 os_defines.h（由 Qt 的头间接引入）已经把 NOMINMAX 定好了，
// 这里再定一次会报警告，所以先判断一下。
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

// 热键 id 只要求在本进程内唯一
constexpr int kHotkeyId = 0x434E; // 'CN'

#ifdef Q_OS_WIN
quint32 toVirtualKey(int key)
{
    // 字母和数字的 Qt::Key 值本身就等于对应的虚拟键码（都是 ASCII）
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return static_cast<quint32>(key);
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return static_cast<quint32>(key);

    // Qt 的 F1..F24 和 Win32 的 VK_F1..VK_F24 都是连续的，平移一下就行
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return VK_F1 + static_cast<quint32>(key - Qt::Key_F1);

    switch (key) {
    case Qt::Key_Space:     return VK_SPACE;
    case Qt::Key_Tab:       return VK_TAB;
    case Qt::Key_Return:
    case Qt::Key_Enter:     return VK_RETURN;
    case Qt::Key_Escape:    return VK_ESCAPE;
    case Qt::Key_Backspace: return VK_BACK;
    case Qt::Key_Insert:    return VK_INSERT;
    case Qt::Key_Delete:    return VK_DELETE;
    case Qt::Key_Home:      return VK_HOME;
    case Qt::Key_End:       return VK_END;
    case Qt::Key_PageUp:    return VK_PRIOR;
    case Qt::Key_PageDown:  return VK_NEXT;
    case Qt::Key_Left:      return VK_LEFT;
    case Qt::Key_Right:     return VK_RIGHT;
    case Qt::Key_Up:        return VK_UP;
    case Qt::Key_Down:      return VK_DOWN;
    default:                return 0;
    }
}
#endif // Q_OS_WIN

} // namespace

GlobalHotkey::GlobalHotkey(QObject *parent) : QObject(parent)
{
    // 装一次，活到析构。没注册热键时 filter 会立刻返回，开销可以忽略。
    if (QCoreApplication *app = QCoreApplication::instance())
        app->installNativeEventFilter(this);
}

GlobalHotkey::~GlobalHotkey()
{
    release();
    if (QCoreApplication *app = QCoreApplication::instance())
        app->removeNativeEventFilter(this);
}

bool GlobalHotkey::toNative(const QKeySequence &shortcut, quint32 *modifiers, quint32 *virtualKey)
{
#ifdef Q_OS_WIN
    if (shortcut.isEmpty())
        return false;

    const QKeyCombination combo = shortcut[0];
    const Qt::KeyboardModifiers mods = combo.keyboardModifiers();

    // MOD_NOREPEAT：按住不放时不重复触发，剪贴板面板被连开好几次很难受
    quint32 nativeMods = MOD_NOREPEAT;
    if (mods.testFlag(Qt::ControlModifier)) nativeMods |= MOD_CONTROL;
    if (mods.testFlag(Qt::AltModifier))     nativeMods |= MOD_ALT;
    if (mods.testFlag(Qt::ShiftModifier))   nativeMods |= MOD_SHIFT;
    if (mods.testFlag(Qt::MetaModifier))    nativeMods |= MOD_WIN;

    // 只有修饰键、没有主键的组合注册不了
    if ((nativeMods & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN)) == 0)
        return false;

    const quint32 vk = toVirtualKey(combo.key());
    if (vk == 0)
        return false;

    *modifiers = nativeMods;
    *virtualKey = vk;
    return true;
#else
    Q_UNUSED(shortcut)
    Q_UNUSED(modifiers)
    Q_UNUSED(virtualKey)
    return false;
#endif
}

bool GlobalHotkey::grab(WId window, const QKeySequence &shortcut)
{
    release();
    m_error.clear();

    if (window == 0) {
        m_error = QStringLiteral("窗口句柄为空");
        return false;
    }

    const QString text = shortcut.toString(QKeySequence::PortableText);
    if (text.isEmpty()) {
        // 最常见的原因是 Windows 键写成 "Win" 了 —— Qt 里要写 "Meta"
        m_error = QStringLiteral("快捷键解析不出来（Windows 键在 Qt 里写作 Meta，写成 Win 会得到空序列）");
        return false;
    }

    quint32 mods = 0;
    quint32 vk = 0;
    if (!toNative(shortcut, &mods, &vk)) {
        m_error = QStringLiteral("不支持这个快捷键：%1（受支持的键见 GlobalHotkey.h）").arg(text);
        return false;
    }

    if (!RegisterHotKey(reinterpret_cast<HWND>(window), kHotkeyId, mods, vk)) {
        const DWORD err = GetLastError();
        if (err == ERROR_HOTKEY_ALREADY_REGISTERED)
            m_error = QStringLiteral("%1 已经被别的程序占用了").arg(text);
        else
            m_error = QStringLiteral("RegisterHotKey 失败，错误码 %1").arg(err);
        return false;
    }

    m_id = kHotkeyId;
    m_window = window;
    return true;
}

void GlobalHotkey::release()
{
    if (m_id == 0)
        return;

    UnregisterHotKey(reinterpret_cast<HWND>(m_window), m_id);
    m_id = 0;
    m_window = 0;
}

bool GlobalHotkey::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
    Q_UNUSED(result)

    if (m_id == 0)
        return false;

    // 注册时绑在具体窗口上的热键经 windows_generic_MSG 过来；
    // 如果哪天改成线程级注册（hwnd 传 0），消息走 windows_dispatcher_MSG。两种都认。
    if (eventType != QByteArrayLiteral("windows_generic_MSG")
        && eventType != QByteArrayLiteral("windows_dispatcher_MSG")) {
        return false;
    }

    const MSG *msg = static_cast<MSG *>(message);
    if (msg->message != WM_HOTKEY || static_cast<int>(msg->wParam) != m_id)
        return false;

    emit activated();
    return true; // 已处理，别再往下传
}

} // namespace ClipNote
