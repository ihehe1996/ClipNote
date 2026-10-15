#pragma once

#include <QChar>
#include <QString>
#include <QStringList>

namespace ClipNote {

// 版本号的解析与比较。
//
// 刻意做得很小：只认「点分数字」，够 GitHub 的 tag 用就行。
// 规则，以及**刻意不做**的部分：
//
//   - 前缀的 v / V 会被剥掉（tag 习惯写成 v0.2.0）
//   - 按 '.' 切开，最多取四段；每段只取**开头连续的阿拉伯数字**
//     （所以 "0.2.0-rc1" 的第三段是 0）
//   - 段数不够的按 0 补齐（"0.2" 和 "0.2.0" 相等）
//   - **预发布后缀不参与比较**："0.2.0-rc1" 和 "0.2.0" 被判为同一个版本
//
// 最后一条是取舍。要正确区分 pre-release 得实现 semver 那一整套优先级规则
// （数字段比字母段小、段多的更大……），而这里自始至终只有「用户手上这个是不是
// 比最新 release 旧」一个问题要回答，多出来的复杂度不值。
// 发版时要么别把 rc 挂在 tag 上，要么接受它会被当成正式版。
struct Version
{
    // 四段足够了：Qt 自己是三位，第四位留给「同版本号重发」这种情况
    int parts[4] = {0, 0, 0, 0};

    // 第四段非 0 才显示。
    //
    // 不能无脑只显示前三段：tag 写成 v8.9.8.1 的时候，界面上显示成 "8.9.8"
    // 会让人以为「更新到的是我已经在用的那个版本」，白白怀疑一遍更新是不是坏的。
    QString toString() const
    {
        if (parts[3] != 0)
            return QStringLiteral("%1.%2.%3.%4")
                .arg(parts[0])
                .arg(parts[1])
                .arg(parts[2])
                .arg(parts[3]);
        return QStringLiteral("%1.%2.%3").arg(parts[0]).arg(parts[1]).arg(parts[2]);
    }
};

// 解析失败返回 false，*out 不动。调用方必须看返回值 —— 解析失败说明
// 「拿什么跟什么比」这件事本身不成立，比出一个「不是更新」的结论毫无意义。
inline bool parseVersion(const QString &text, Version *out)
{
    QString s = text.trimmed();
    if (s.startsWith(QLatin1Char('v')) || s.startsWith(QLatin1Char('V')))
        s.remove(0, 1);
    if (s.isEmpty())
        return false;

    const QStringList segments = s.split(QLatin1Char('.'), Qt::KeepEmptyParts);

    Version parsed;
    int filled = 0;
    for (const QString &segment : segments) {
        if (filled >= 4)
            break;

        // 只取开头连续的数字。"0-rc1" 取到 "0"，"rc1" 一个都取不到
        int digits = 0;
        while (digits < segment.size() && segment.at(digits).isDigit())
            ++digits;

        // 这一段开头就不是数字，后面的段再解析也没有意义了（"1.x.3" 当 1 看）
        if (digits == 0)
            break;

        parsed.parts[filled++] = segment.left(digits).toInt();
    }

    // 一段都没解析出来，那就是个不合格的版本号
    if (filled == 0)
        return false;

    if (out)
        *out = parsed;
    return true;
}

// 三值比较，和 qstrcmp 一个约定：a < b 返回负，相等返回 0，a > b 返回正
inline int compareVersions(const Version &a, const Version &b)
{
    for (int i = 0; i < 4; ++i) {
        if (a.parts[i] != b.parts[i])
            return a.parts[i] < b.parts[i] ? -1 : 1;
    }
    return 0;
}

// candidate 是不是比 current 新。相等不算新 —— 否则每次检查都会提示一遍同样的更新。
inline bool isNewerVersion(const Version &candidate, const Version &current)
{
    return compareVersions(candidate, current) > 0;
}

} // namespace ClipNote
