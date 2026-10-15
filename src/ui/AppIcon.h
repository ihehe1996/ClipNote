#pragma once

#include <QIcon>

namespace ClipNote {

// 程序图标：resources/clipnote.png，通过 resources/resources.qrc 编进程序。
// 托盘图标和自绘标题栏左上角用的是同一个，所以抽出来共用。
//
// 换图标：直接替换 resources/clipnote.png 再重新构建即可（AUTORCC 已开）。
// 建议用正方形、带透明通道的 PNG，256px 以上 —— 它要缩到 16px 去做托盘图标。
QIcon appIcon();

} // namespace ClipNote
