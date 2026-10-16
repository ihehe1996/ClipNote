#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QImage>
#include <QPixmap>
#include <QString>
#include <QVector>

namespace ClipNote {

struct Clip
{
    enum Kind { Text, Image };

    Kind kind = Text;

    QString text;        // kind == Text
    QImage image;        // kind == Image：原图。粘贴回去要用它，所以不能只留缩略图
    QPixmap thumb;       // kind == Image：列表里显示的小图，预先缩好，免得每次重绘都缩一遍
    uint imageHash = 0;  // kind == Image：去重用。逐像素比对太贵

    // 收藏。收藏项**不受 kMaxItems 上限影响**，也不会被「清空历史」清掉 ——
    // 收藏就是特意留下来的，被新记录挤掉就没意义了。
    bool favorite = false;

    QDateTime capturedAt;
};

// 剪贴板历史。纯数据，刻意不碰 QClipboard —— 这是它能被 --selftest 无头构造、
// 直接调用 pushText()/pushImage() 验证的前提。负责采集剪贴板的是 ClipboardWatcher。
//
// 为什么是 QAbstractListModel 而不是兄弟项目 EmShell 那种「普通 QObject + 手搭
// 一列 QFrame」：EmShell 的 historyChanged 是低频的（连一次服务器才触发一次），
// 而这里整个会话里每次 Ctrl+C 都会触发。EmShell 的 rebuild() 会把所有行控件
// 删掉重建，放到剪贴板的频率上就是 O(n) 的控件抖动。
class ClipStore : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        TextRole = Qt::UserRole + 1, // 完整原文（列表里显示的是折过行的摘要）
        CapturedAtRole,
        KindRole,                    // Clip::Kind
        ThumbRole,                   // 图片的缩略图（QPixmap），列表显示用
        ImageRole,                   // 图片的原图（QImage），粘贴回去用
        FavoriteRole,                // 收藏（bool）。**只能往后加**，插在中间会让后面所有 role 错位
    };

    // 历史上限，超出后从最旧的开始丢
    static constexpr int kMaxItems = 200;

    // 收进来的图片像素上限。历史全在内存里，一张大图就是几十 MB，
    // 超过这个量级的直接不收（8K 截图约 33MP，先挡住那种）。
    static constexpr qint64 kMaxImagePixels = 40'000'000;

    explicit ClipStore(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;

    // 新内容进历史：空内容忽略；已经在历史里的挪到最前面，而不是重复存一条。
    void pushText(const QString &text);
    void pushImage(const QImage &image);

    // 删掉一条。越界就什么都不做，也不发信号。
    void removeAt(int row);

    // 「清空历史」：只删**非收藏**的条目，收藏的留着（右键菜单里那一项就是它）。
    // 收藏项只能一条条手动取消收藏后删，见 clearFavorites()。
    void clearHistory();

    // 「取消全部收藏」：只把 favorite 标记清掉，**不删条目**。
    void clearFavorites();

    // 从磁盘恢复用：整批装进去，顺序、时间戳、收藏标记照抄。
    // **刻意不走去重/置顶那套** —— 文件里的顺序就是展示顺序，而且时间戳不该被刷成「现在」。
    void restoreTexts(const QVector<Clip> &items);

    bool isFavorite(int row) const;
    void setFavorite(int row, bool favorite);

    // 越界返回空串，不崩
    QString textAt(int row) const;

    // 图片内容指纹。写剪贴板的一方和收剪贴板的一方都算它，才能判断
    // 「这次剪贴板变化是不是我们自己写的」。按缩略图算而不是原图：
    // 原图可能有几十 MB，每次粘贴都哈希一遍会卡顿。
    static uint imageHash(const QImage &image);

private:
    int indexOfText(const QString &text) const;
    int indexOfImage(uint hash) const;
    void insertOrPromote(const Clip &clip, int existingRow);
    void trimToLimit();

    QVector<Clip> m_items;
};

} // namespace ClipNote
