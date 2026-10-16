#pragma once

#include <QString>

namespace ClipNote {

class ClipStore;

// 剪贴板历史的落盘与读回。
//
// **只存文本**。图片不落盘：一张截图就是几 MB，历史里攒几十张能把磁盘和启动时间
// 都拖下水，而且截图随时可以重新截 —— 它不像文字那样丢了就找不回来。
// 所以收藏一张图片只在本次运行内有效，重启后它不会出现在收藏页。
//
// 格式是 JSON（`{"version":1,"items":[{"text":…,"at":…,"favorite":…}]}`），
// 数组顺序就是展示顺序（最新在前）。**没有用 SQLite** —— 存的是 ≤200 条文本、
// 撑死几百 KB 的东西，为它多带一条运行期依赖（Qt6Sql + sqldrivers 插件，
// 打包和「裸 exe 更新」都要跟着操心）不划算。
//
// 换成别的存储只要改这两个函数的实现，外面看不见。
bool saveHistory(const QString &path, const ClipStore &store, QString *error);

// 读不回来（文件不在、坏了、版本不认识）就当空历史启动，**不弹任何提示** ——
// 历史丢了大不了重来，为它弹个对话框打扰用户不值当。error 只是留给自检看的。
bool loadHistory(const QString &path, ClipStore *store, QString *error);

// 历史文件该放哪：%APPDATA%\ClipNote\ClipNote\history.json。
QString historyFilePath();

} // namespace ClipNote
