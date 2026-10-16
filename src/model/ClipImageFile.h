#pragma once

#include <QImage>
#include <QSet>
#include <QString>

class QPixmap;

namespace ClipNote {

// 图片条目的落盘。每个图片存**两个**文件：
//
//   <hash>.png         原图（PNG 无损，粘回去和原来一模一样）
//   <hash>.thumb.png   96px 缩略图
//
// 分开存是**懒加载**的前提：启动时只读那些几 KB 的缩略图，原图要等到「看大图」或
// 「要粘贴」时才读。不分开的话，启动就得把每一张大图都解出来，一张 4K 截图解出来
// 是 33 MB、解码还要几百毫秒，攒几十张就废了。
//
// `<hash>` 是 `ClipStore::imageHash()` 的十六进制值。用它当文件名有两个讲究：
//   · **跨进程稳定** —— 不稳定的话重启后名字就变了，永远找不回自己的图
//     （它在 qhashfunctions.h 里标了 Q_DECL_PURE_FUNCTION，不读任何全局状态）
//   · **和去重口径一致** —— ClipStore 本来就用同一个哈希去重，所以「文件名撞了」
//     和「两条被当成同一条」是同一件事，不会出现 A 条目指向 B 的图
//
// 这几个函数都**显式收目录**，不自己去问 %APPDATA%：这样自检能指着临时目录跑，
// 不会碰到用户的真实数据。（默认目录见 defaultImagesDir。）
//
// 所有操作都幂等：文件不在就当删过了，读不到就返回空。

QString defaultImagesDir();

QString imagePathFor(const QString &dir, uint hash);
QString thumbPathFor(const QString &dir, uint hash);

bool saveImage(const QString &dir, uint hash, const QImage &image, const QPixmap &thumb,
               QString *error = nullptr);

// 读不到（文件不在、坏了）返回空对象，不报错 —— 调用方按「这条失效」处理。
QImage loadImage(const QString &dir, uint hash);
QPixmap loadThumb(const QString &dir, uint hash);

void removeImage(const QString &dir, uint hash);

// 扫一遍目录，把不在 live 里的文件删掉。
// 用途是**兜底**：程序如果在「写完图片文件」和「写 history.json」之间崩掉，
// 就会留下一个没人引用的图片文件，靠这个在下次启动时清掉。
void removeOrphanImages(const QString &dir, const QSet<uint> &live);

} // namespace ClipNote
