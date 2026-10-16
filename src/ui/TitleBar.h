#pragma once

#include <QPoint>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QLabel;
class QToolButton;
QT_END_NAMESPACE

namespace ClipNote {

// 自绘的标题栏：图标 + 标题 + 齿轮菜单 + 关闭按钮，空白处可以拖动窗口。
//
// 为什么要自绘：Windows 11 的标题栏由 DWM 合成，应用在 WM_NCPAINT 里画上去的东西会被
// 直接盖掉（实测过，见 CLAUDE.md 的「架构」一节），所以想要非原生的外观就只能整条自己画，
// 窗口随之改成无边框。
//
// 代价，相对原生标题栏：没有系统提供的拖拽调大小（自己补回来了，见
// platform/WindowResizer.h），也没有原生投影/圆角。换来的是外观完全可控。
class TitleBar : public QWidget
{
    Q_OBJECT

public:
    explicit TitleBar(QWidget *parent = nullptr);

signals:
    void closeRequested();
    // 齿轮菜单里选出来的。标题栏不自己去开对话框 —— 那些窗口的父子关系和生命周期
    // 归 MainWindow 管，这里只管把意图转达上去。
    void checkUpdateRequested();
    void aboutRequested();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void showGearMenu();

    QLabel *m_title = nullptr;
    QToolButton *m_gear = nullptr;

    // startSystemMove() 万一不受理（本窗口是「永不激活」的），就退回自己跟鼠标
    bool m_dragging = false;
    QPoint m_dragOffset;
};

} // namespace ClipNote
