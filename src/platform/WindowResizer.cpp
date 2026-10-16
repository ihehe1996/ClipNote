#include "platform/WindowResizer.h"

#include <QWidget>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
// GET_X_LPARAM / GET_Y_LPARAM 在这里，不在 windows.h 里。
// 必须用它们而不是 LOWORD/HIWORD：那两个是无符号的，副屏在主屏左边/上边时
// 坐标是负数，取出来会变成一个很大的正数。
#  include <windowsx.h>
#endif

namespace ClipNote {

#ifdef Q_OS_WIN

namespace {

// 边缘命中带的宽度，物理像素。取系统自己那个值（边框 + 内边距），它天然跟着 DPI 走；
// 算出来是 0 的话给个下限，免得边缘变成一条摸不到的线。
int borderWidth()
{
    const int width = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
    return width > 4 ? width : 4;
}

QPoint cursorInPhysicalPixels()
{
    POINT p;
    GetCursorPos(&p);
    return QPoint(p.x, p.y);
}

// 拖动过程中光标的形状。必须自己按着：命中测试是按**当前**光标位置算的，
// 手拖快了两下就跑到离边缘很远的地方，那时系统会认为已经不是边缘，光标跳回箭头。
LPCTSTR cursorIdForHitCode(int hitCode)
{
    switch (hitCode) {
    case HTLEFT:
    case HTRIGHT:
        return IDC_SIZEWE;
    case HTTOP:
    case HTBOTTOM:
        return IDC_SIZENS;
    case HTTOPLEFT:
    case HTBOTTOMRIGHT:
        return IDC_SIZENWSE;
    default:
        return IDC_SIZENESW;
    }
}

} // namespace

#endif // Q_OS_WIN

WindowResizer::WindowResizer(QWidget *window) : m_window(window) {}

bool WindowResizer::touchesLeft(Edge e) const
{
    return e == Edge::Left || e == Edge::TopLeft || e == Edge::BottomLeft;
}

bool WindowResizer::touchesRight(Edge e) const
{
    return e == Edge::Right || e == Edge::TopRight || e == Edge::BottomRight;
}

bool WindowResizer::touchesTop(Edge e) const
{
    return e == Edge::Top || e == Edge::TopLeft || e == Edge::TopRight;
}

bool WindowResizer::touchesBottom(Edge e) const
{
    return e == Edge::Bottom || e == Edge::BottomLeft || e == Edge::BottomRight;
}

int WindowResizer::hitCode(Edge e)
{
#ifdef Q_OS_WIN
    switch (e) {
    case Edge::Left:         return HTLEFT;
    case Edge::Right:        return HTRIGHT;
    case Edge::Top:          return HTTOP;
    case Edge::Bottom:       return HTBOTTOM;
    case Edge::TopLeft:      return HTTOPLEFT;
    case Edge::TopRight:     return HTTOPRIGHT;
    case Edge::BottomLeft:   return HTBOTTOMLEFT;
    case Edge::BottomRight:  return HTBOTTOMRIGHT;
    case Edge::None:         break;
    }
#endif
    return 0;
}

WindowResizer::Edge WindowResizer::edgeAt(int screenX, int screenY) const
{
#ifdef Q_OS_WIN
    RECT r;
    if (!GetWindowRect(reinterpret_cast<HWND>(m_window->winId()), &r))
        return Edge::None;

    const int border = borderWidth();
    const bool left   = screenX >= r.left && screenX < r.left + border;
    const bool right  = screenX < r.right && screenX >= r.right - border;
    const bool top    = screenY >= r.top && screenY < r.top + border;
    const bool bottom = screenY < r.bottom && screenY >= r.bottom - border;

    // 角优先：一个点同时满足「左」和「上」时必须是 TopLeft 而不是 Left
    if (top && left)     return Edge::TopLeft;
    if (top && right)    return Edge::TopRight;
    if (bottom && left)  return Edge::BottomLeft;
    if (bottom && right) return Edge::BottomRight;
    if (left)            return Edge::Left;
    if (right)           return Edge::Right;
    if (top)             return Edge::Top;
    if (bottom)          return Edge::Bottom;
#else
    Q_UNUSED(screenX);
    Q_UNUSED(screenY);
#endif
    return Edge::None;
}

void WindowResizer::beginResize(Edge edge)
{
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(m_window->winId());

    RECT r;
    if (!GetWindowRect(hwnd, &r))
        return;

    m_edge = edge;
    m_startLeft = r.left;
    m_startTop = r.top;
    m_startRight = r.right;
    m_startBottom = r.bottom;
    m_startCursor = cursorInPhysicalPixels();

    SetCapture(hwnd);
#else
    Q_UNUSED(edge);
#endif
}

void WindowResizer::updateResize()
{
#ifdef Q_OS_WIN
    const QPoint now = cursorInPhysicalPixels();
    const int dx = now.x() - m_startCursor.x();
    const int dy = now.y() - m_startCursor.y();

    int left = m_startLeft;
    int top = m_startTop;
    int right = m_startRight;
    int bottom = m_startBottom;

    if (touchesLeft(m_edge))   left += dx;
    if (touchesRight(m_edge))  right += dx;
    if (touchesTop(m_edge))    top += dy;
    if (touchesBottom(m_edge)) bottom += dy;

    // 夹住下限。往左/上拖过头时改的是 left/top（对边不动），
    // 否则窗口会一边缩到最小一边自己往右上角游走。
    // 最小尺寸是按逻辑像素声明的，这里换成物理像素再比 —— 本函数全程物理像素。
    const qreal dpr = m_window->devicePixelRatioF();
    const int minW = qRound(m_window->minimumWidth() * dpr);
    const int minH = qRound(m_window->minimumHeight() * dpr);
    if (right - left < minW) {
        if (touchesLeft(m_edge))
            left = right - minW;
        else
            right = left + minW;
    }
    if (bottom - top < minH) {
        if (touchesTop(m_edge))
            top = bottom - minH;
        else
            bottom = top + minH;
    }

    // SWP_NOACTIVATE：这个窗口永不激活，缩放也不能把它激活。
    // SWP_NOZORDER：它是永远置顶的，别让它因为一次缩放掉到别的窗口后面。
    SetWindowPos(reinterpret_cast<HWND>(m_window->winId()), nullptr,
                 left, top, right - left, bottom - top,
                 SWP_NOACTIVATE | SWP_NOZORDER);
#endif
}

void WindowResizer::endResize()
{
    m_edge = Edge::None;
#ifdef Q_OS_WIN
    ReleaseCapture();
#endif
}

bool WindowResizer::handleMessage(const QByteArray &eventType, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    if (!m_window)
        return false;
    // Qt 把发往顶层窗口的消息以这个类型送过来（见 GlobalHotkey 里的同类判断）
    if (eventType != QByteArrayLiteral("windows_generic_MSG"))
        return false;

    MSG *msg = static_cast<MSG *>(message);

    switch (msg->message) {
    case WM_NCHITTEST: {
        // lParam 是**屏幕**坐标。用带符号的 GET_X_LPARAM/GET_Y_LPARAM，不能拿
        // LOWORD/HIWORD 去拼 —— 副屏在主屏左边或上边时坐标是负的，按无符号解释会翻车。
        const Edge edge = edgeAt(GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam));
        if (edge == Edge::None)
            return false;   // 交回 Qt/DefWindowProc 得到 HTCLIENT，子控件照常收鼠标

        // 这里**只**是为了让系统给出缩放光标，缩放本身在下面 WM_NCLBUTTONDOWN 里自己接管。
        // 注意必须写 *result：不写的话它是 0（HTNOWHERE），整扇窗口会变成点击穿透。
        *result = hitCode(edge);
        return true;
    }

    case WM_NCLBUTTONDOWN: {
        const Edge edge = edgeAt(GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam));
        if (edge == Edge::None)
            break;   // 标题栏那种（HTCAPTION）不归这里管，照旧让 Qt 去移动窗口

        beginResize(edge);
        // **把消息吞掉**：不吞的话 DefWindowProc 会把它转成 WM_SYSCOMMAND / SC_SIZE，
        // 那就是要避开的系统缩放模态循环（不激活的窗口跑它时画面不重绘）。
        return true;
    }

    case WM_MOUSEMOVE:
    case WM_NCMOUSEMOVE:   // 光标压在划出来的边缘带上时走这条
        if (m_edge != Edge::None) {
            updateResize();
            return true;
        }
        break;

    case WM_LBUTTONUP:
    case WM_NCLBUTTONUP:
        if (m_edge != Edge::None) {
            endResize();
            return true;
        }
        break;

    case WM_CAPTURECHANGED:
        // 捕获被别的东西抢走（弹了菜单之类）：别留下「还以为在拖」的状态
        m_edge = Edge::None;
        break;

    case WM_SETCURSOR:
        if (m_edge != Edge::None) {
            SetCursor(LoadCursorW(nullptr, cursorIdForHitCode(hitCode(m_edge))));
            *result = TRUE;
            return true;
        }
        break;

    default:
        break;
    }
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
    Q_UNUSED(result);
#endif
    return false;
}

} // namespace ClipNote
