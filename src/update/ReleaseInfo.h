#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QVector>

namespace ClipNote {

// GitHub release 里的一个附件
struct ReleaseAsset
{
    QString name;
    QString downloadUrl;
    qint64 size = -1;     // 字节；-1 表示响应里没给
    QString sha256;       // 从 digest 字段来的，可能是空的（老 release 没有）
};

// 从 GitHub 的 /repos/{owner}/{repo}/releases/latest 里挑出来的那几个字段，
// 其余的（作者、reactions、tarball 地址……）一律不存。
struct ReleaseInfo
{
    QString tagName;
    QString title;
    QString notes;        // release 正文，markdown，原样显示
    QString pageUrl;      // html_url，给「在浏览器里打开」用
    QDateTime publishedAt;
    QVector<ReleaseAsset> assets;
};

// 解析 GitHub 的 release JSON。**纯函数，不碰网络**，所以自检里能直接喂一份
// 抓下来的真实响应对照验证 —— 这是自检唯一能覆盖到更新逻辑的地方。
// 失败时返回 false 并在 *error 里写原因（*error 允许为空）。
bool parseGitHubRelease(const QByteArray &json, ReleaseInfo *out, QString *error);

// 从附件里挑出用来升级的那一个，挑不到返回 nullptr。
//
// 优先级：**.zip 优于 .exe**。理由是 zip 能整包带上新增的依赖 DLL
// （比如这个版本刚开始用 Qt6Network，老用户的目录里就没有那个 dll），
// 而裸 exe 只能换主程序、留着老的一套 DLL。
// 同类里取第一个匹配的，所以发版时的命名自己拿捏。
//
// **名字里带 setup / install 的一律跳过**。发安装包之后 Release 里会同时有
// 便携 zip 和安装程序 exe，而后者也是 .exe —— 被挑中的话会把用户的主程序
// 换成安装程序本身。详见实现里的注释。
const ReleaseAsset *pickUpdateAsset(const ReleaseInfo &info);

} // namespace ClipNote
