#pragma once

#include <QDialog>

namespace ClipNote {

// 「关于 ClipNote」。
//
// 注意窗口标志：面板自己是无边框 + 永远置顶的，这个对话框挂在它下面，
// 所以也必须置顶，否则会被面板盖住 —— 那看着就像点了没反应。
class AboutDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AboutDialog(QWidget *parent = nullptr);
};

} // namespace ClipNote
