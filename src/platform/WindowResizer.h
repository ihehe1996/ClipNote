#pragma once

#include <QByteArray>
#include <QPoint>
#include <QtGlobal>

QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

namespace ClipNote {

// 无边框窗口的「拖边缘改大小」。
//
// 为什么需要它：面板的标题栏是自绘的，窗口因此是无边框的（原因见 ui/TitleBar.h），
// 系统的缩放边框也一并没了 —— 面板当初被做成固定尺寸就是因为这个。
//
// 为什么不让系统自己缩：那条路要同时满足两个条件，两个都不划算 ——
//
//   1. 得给窗口补上 WS_THICKFRAME，否则 Win10/11 上缩放根本不生效（缩放的模态循环
//      有一道 thickframe 门禁）。补上之后会多出系统画的边框、客户区往里缩一圈，
//      还得跟 Qt 抢窗口样式的方向盘。
//   2. WS_EX_NOACTIVATE 的窗口跑系统的缩放循环时**画面不重绘**，要松手才跳到最终大小。
//      而 WS_EX_NOACTIVATE 是这个面板的支柱性质（不抢焦点、单击即粘贴都依赖它），
//      为了拖大窗口去动它不划算。
//
// 所以这里自己算几何：
//
//   WM_NCHITTEST      压在边缘 → 写 *result = HTLEFT 这类命中码（只为拿到系统的缩放光标）
//   WM_NCLBUTTONDOWN  命中码是边缘 → 记下起始矩形与光标、SetCapture、**把消息吞掉**
//                     （不吞的话 DefWindowProc 会把它转成 SC_SIZE 模态循环）
//   WM_MOUSEMOVE      拖动中 → 按光标位移算新矩形，SetWindowPos(SWP_NOACTIVATE)
//   WM_LBUTTONUP      收工
//
// 每帧都走 Qt 的正常重绘，所以拖动过程是跟手的。
//
// 全程**物理像素**：WM_NCHITTEST 的 lParam 就是屏幕坐标，配 GetWindowRect /
// SetWindowPos / GetCursorPos 用，中间不碰 Qt 的逻辑像素，省掉一整套 DPI 换算。
class WindowResizer
{
public:
    // window 是被拖的那个顶层窗口。构造时只是记下指针，不碰原生句柄。
    explicit WindowResizer(QWidget *window);

    // 把一条原生消息交进来。处理了就返回 true（调用方直接把自己的 nativeEvent
    // 返回 true 即可）。不是 Windows 的消息一律不碰。
    bool handleMessage(const QByteArray &eventType, void *message, qintptr *result);

private:
    enum class Edge { None, Left, Right, Top, Bottom,
                      TopLeft, TopRight, BottomLeft, BottomRight };

    Edge edgeAt(int screenX, int screenY) const;
    static int hitCode(Edge e);
    bool touchesLeft(Edge e) const;
    bool touchesRight(Edge e) const;
    bool touchesTop(Edge e) const;
    bool touchesBottom(Edge e) const;

    void beginResize(Edge edge);
    void updateResize();
    void endResize();

    QWidget *m_window = nullptr;

    // 拖动中的状态。起始矩形按四条边分开存，免得为了一个 RECT 把 windows.h
    // 引进这个头文件里（AutoPaste.h 也是这么隔离的）。都是物理像素。
    Edge m_edge = Edge::None;
    int m_startLeft = 0;
    int m_startTop = 0;
    int m_startRight = 0;
    int m_startBottom = 0;
    QPoint m_startCursor;
};

} // namespace ClipNote
