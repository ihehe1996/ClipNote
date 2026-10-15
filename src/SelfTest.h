#pragma once

#include <QStringList>

namespace ClipNote {

// 命令行自检。
//
//     ClipNote.exe --selftest
//
// 程序按 WIN32（GUI 子系统）构建，没有控制台，qDebug() 的输出哪儿也去不了，
// 所以结果写进当前目录的 selftest.log，退出码非 0 表示有检查项没过。
//
// 用的是真环境而不是 mock：托盘向系统要，默认全局热键真的注册一次再注销，
// 剪贴板真的写一次再读回来（读完把用户原来的内容还原回去），
// 剪贴板历史的存储逻辑直接构造 store 灌数据验证。

// **安全边界**：自检可以读系统状态、可以往返一次剪贴板；绝不能往任何窗口注入按键。
// 所以本文件不包含 platform/AutoPaste.h。自动粘贴只能手动验证 ——
// 日志里会留一行 [ -- ] 说明它没被测，而不是静默略过。
bool isSelfTestRequest(const QStringList &arguments);
int  runSelfTest(const QStringList &arguments);

} // namespace ClipNote
