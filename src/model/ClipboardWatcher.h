#pragma once

#include <QObject>
#include <QString>

namespace ClipNote {

class ClipStore;

// 采集系统剪贴板。
//
// 只做一件事：把剪贴板里新出现的文本/图片喂给 ClipStore。判断和存放的活都在 store 里 ——
// 这样 store 才能脱离剪贴板被 --selftest 验证，而本类天然不参与自检。
class ClipboardWatcher : public QObject
{
    Q_OBJECT

public:
    explicit ClipboardWatcher(ClipStore *store, QObject *parent = nullptr);

    // 告诉 watcher：紧接着这次剪贴板变化是**我们自己**写进去的（用户点了面板里的条目），
    // 别当成用户新复制的内容收进历史。
    //
    // 不这么做的话，store 会把这一条挪到列表最前面 —— 面板还不关闭，
    // 列表就在用户眼皮底下重排，他下一次点击会落到别的条目上。
    //
    // 记的是**内容**而不是一个「忽略下一次」的裸标志：万一 setText 没触发
    // dataChanged（比如剪贴板里本来就是这个内容），裸标志会一直挂着，
    // 反而把之后第一次真正的复制吃掉。
    void suppressChange(const QString &text);
    void suppressImageChange(uint hash);

private:
    void onClipboardChanged();

    ClipStore *m_store = nullptr;
    QString m_suppressedText;
    uint m_suppressedImageHash = 0;
};

} // namespace ClipNote
