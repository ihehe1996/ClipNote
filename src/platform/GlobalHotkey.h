#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QString>
#include <QtGui/qwindowdefs.h> // WId（Qt 6 里是 quintptr；Qt 5 时代这个头在 QtCore）

QT_BEGIN_NAMESPACE
class QKeySequence;
QT_END_NAMESPACE

namespace ClipNote {

// 全局热键（系统级快捷键）。
//
// 为什么不用 QShortcut：QShortcut 只在应用窗口有焦点时才触发，而托盘常驻的
// 程序绝大多数时间没有焦点，必须用 Win32 的 RegisterHotKey 让系统在按键按下时
// 把 WM_HOTKEY 投递给我们的窗口，再由 QAbstractNativeEventFilter 接住，
// 转成 activated() 信号。
//
//     m_hotkey = new GlobalHotkey(this);
//     if (m_hotkey->grab(window->winId(), QKeySequence(QStringLiteral("Alt+Q"))))
//         connect(m_hotkey, &GlobalHotkey::activated, ...);
//     else
//         qWarning() << m_hotkey->errorString();
//
// 两点要记住：
//
// 1. 热键是系统级独占的。同一个组合已被别的进程占用时 grab() 返回 false，
//    原因在 errorString() 里，此时应当降级（让托盘菜单接手）而不是退出。
// 2. window 传的是注册时绑定的顶层窗口句柄（QWidget::winId()）。窗口一旦销毁，
//    系统会自动注销热键，所以那个窗口必须活到热键失效为止。
//
// 受支持的按键：A–Z、0–9、F1–F24、空格、Tab、回车、Esc、退格、
// Insert/Delete/Home/End/PageUp/PageDown、四个方向键。
// 需要其它键时在 GlobalHotkey.cpp 的 toVirtualKey() 里加一条。
//
// 写快捷键字符串时，Windows 键是 "Meta" 不是 "Win"：
// QKeySequence("Win+Shift+C") 会解析成空序列，注册必然失败。
class GlobalHotkey : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    explicit GlobalHotkey(QObject *parent = nullptr);
    ~GlobalHotkey() override;

    // 注册热键。重复调用会先注销上一个。
    bool grab(WId window, const QKeySequence &shortcut);
    void release();

    bool isGrabbed() const { return m_id != 0; }
    QString errorString() const { return m_error; }

    // 把 Qt 快捷键翻译成 Win32 的 (修饰键, 虚拟键码)。翻译不了时返回 false。
    static bool toNative(const QKeySequence &shortcut, quint32 *modifiers, quint32 *virtualKey);

signals:
    void activated();

protected:
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    int m_id = 0;        // RegisterHotKey 用的 id；0 表示当前没注册
    WId m_window = 0;    // 热键绑定的窗口
    QString m_error;
};

} // namespace ClipNote
