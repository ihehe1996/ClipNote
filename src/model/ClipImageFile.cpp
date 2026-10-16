#include "model/ClipImageFile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPixmap>
#include <QStandardPaths>

namespace ClipNote {

namespace {

QString fileNameFor(uint hash, bool thumb)
{
    // 定长十六进制，补零 —— 不然两张图的文件名可能因为位数不同而看起来像前缀关系
    return QStringLiteral("%1%2.png")
        .arg(hash, 8, 16, QLatin1Char('0'))
        .arg(thumb ? QStringLiteral(".thumb") : QString());
}

} // namespace

QString defaultImagesDir()
{
    // 和 history.json 摆在同一个应用数据目录下（Windows 上就是 %APPDATA%\ClipNote\ClipNote）
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty())
        return {};
    return QDir(dir).filePath(QStringLiteral("images"));
}

QString imagePathFor(const QString &dir, uint hash)
{
    return dir.isEmpty() ? QString() : QDir(dir).filePath(fileNameFor(hash, false));
}

QString thumbPathFor(const QString &dir, uint hash)
{
    return dir.isEmpty() ? QString() : QDir(dir).filePath(fileNameFor(hash, true));
}

bool saveImage(const QString &dir, uint hash, const QImage &image, const QPixmap &thumb,
               QString *error)
{
    if (dir.isEmpty() || hash == 0 || image.isNull()) {
        if (error)
            *error = QStringLiteral("图片落盘：目录为空 / 哈希为 0 / 图是空的");
        return false;
    }

    if (!QDir().mkpath(dir)) {
        if (error)
            *error = QStringLiteral("建不出图片目录：%1").arg(dir);
        return false;
    }

    // PNG 无损。截图这类内容用 PNG 压缩率很好，而且**粘回去和原图逐像素一致**——
    // 用 JPEG 的话文字边缘会糊，粘贴出去的就不是用户复制的那张图了。
    if (!image.save(imagePathFor(dir, hash), "PNG")) {
        if (error)
            *error = QStringLiteral("原图写不进去：%1").arg(imagePathFor(dir, hash));
        return false;
    }

    // 缩略图失败不算致命：列表上少张小图，比整条图丢了强
    if (!thumb.isNull())
        thumb.save(thumbPathFor(dir, hash), "PNG");

    return true;
}

QImage loadImage(const QString &dir, uint hash)
{
    if (dir.isEmpty() || hash == 0)
        return {};
    // 注意：这里**不做缓存**。缓存由 ClipStore 管（它知道哪一行要了什么），
    // 在这一层缓存会让「用户删了文件」这种情况变得难以推理。
    return QImage(imagePathFor(dir, hash));
}

QPixmap loadThumb(const QString &dir, uint hash)
{
    if (dir.isEmpty() || hash == 0)
        return {};
    return QPixmap(thumbPathFor(dir, hash));
}

void removeImage(const QString &dir, uint hash)
{
    if (dir.isEmpty() || hash == 0)
        return;
    // 幂等：不在就当删过了，QFile::remove 返回 false 也不管
    QFile::remove(imagePathFor(dir, hash));
    QFile::remove(thumbPathFor(dir, hash));
}

void removeOrphanImages(const QString &dir, const QSet<uint> &live)
{
    if (dir.isEmpty())
        return;

    QDir images(dir);
    if (!images.exists())
        return;

    // 先把「该有的文件名」列出来，然后目录里凡是没在这一堆里的都删掉
    QSet<QString> keep;
    for (uint hash : live) {
        keep.insert(fileNameFor(hash, false));
        keep.insert(fileNameFor(hash, true));
    }

    const QStringList files =
        images.entryList({QStringLiteral("*.png")}, QDir::Files | QDir::NoDotAndDotDot);
    for (const QString &name : files) {
        if (!keep.contains(name))
            QFile::remove(images.filePath(name));
    }
}

} // namespace ClipNote
