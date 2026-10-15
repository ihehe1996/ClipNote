#include "model/ClipboardWatcher.h"

#include "model/ClipStore.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>

namespace ClipNote {

ClipboardWatcher::ClipboardWatcher(ClipStore *store, QObject *parent)
    : QObject(parent), m_store(store)
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard || !m_store)
        return;

    connect(clipboard, &QClipboard::dataChanged, this, &ClipboardWatcher::onClipboardChanged);

    // 启动时先把剪贴板里已有的内容收进来，否则要等用户复制一次才出现第一条
    onClipboardChanged();
}

void ClipboardWatcher::suppressChange(const QString &text)
{
    m_suppressedText = text;
    m_suppressedImageHash = 0;
}

void ClipboardWatcher::suppressImageChange(uint hash)
{
    m_suppressedImageHash = hash;
    m_suppressedText.clear();
}

void ClipboardWatcher::onClipboardChanged()
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return;

    // 图片优先：剪贴板里同时有图和文本时（比如从浏览器复制的图片），
    // 文本往往是空的或者只是个 URL，收文本没意义。
    const QMimeData *mime = clipboard->mimeData();
    if (mime && mime->hasImage()) {
        const QImage image = clipboard->image();
        const uint hash = ClipStore::imageHash(image);

        const bool selfWritten = m_suppressedImageHash != 0 && hash == m_suppressedImageHash;
        m_suppressedImageHash = 0;
        m_suppressedText.clear();

        if (!selfWritten && !image.isNull())
            m_store->pushImage(image);
        return;
    }

    // 不是文本也不是图片的内容（比如文件），text() 取出来是空串，pushText 会直接忽略。
    const QString text = clipboard->text();
    const bool selfWritten = !m_suppressedText.isEmpty() && text == m_suppressedText;
    m_suppressedText.clear();
    m_suppressedImageHash = 0;

    if (!selfWritten)
        m_store->pushText(text);
}

} // namespace ClipNote
