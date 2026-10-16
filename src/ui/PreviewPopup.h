#pragma once

#include <QWidget>

QT_BEGIN_NAMESPACE
class QImage;
class QLabel;
QT_END_NAMESPACE

namespace ClipNote {

// 鼠标停在图片条目的缩略图上时，弹出来显示大图。
//
// **只管图片**。文本的悬停提示用的是模型里那个 Qt::ToolTipRole，由列表弹**原生气泡**
//（`QStyledItemDelegate::helpEvent`），不需要这里插手 —— 之前两边都弹过，同一段文字
// 会出来两次。
//
// 图片没法交给原生气泡：QToolTip 只吃文本，塞不进一张位图（拿 HTML 的 <img> 绕的话
// 得转成临时文件或 data URI）。所以只有这一个窗口是自绘的。
//
// 窗口标志用 Qt::ToolTip：它天然不抢焦点、不进任务栏，正好和面板自己的性质一致
//（面板是 WS_EX_NOACTIVATE 的，预览要是抢了焦点就会把用户原来那个程序的光标顶掉）。
//
// 它是**只读**的：弹出后不吃鼠标事件，鼠标一动就收起来（收起由 MainWindow 管）。
class PreviewPopup : public QWidget
{
    Q_OBJECT

public:
    explicit PreviewPopup(QWidget *parent = nullptr);

    void showImage(const QImage &image);

    // 摆在面板外侧：优先右边，右边放不下就翻到左边；纵向对着光标，再夹进屏幕。
    void showNextTo(const QWidget *panel, const QPoint &cursor);

private:
    void showAt(const QPoint &pos);

    QLabel *m_content = nullptr;
};

} // namespace ClipNote
