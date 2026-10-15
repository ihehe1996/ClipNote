#include "update/UpdateInstaller.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryFile>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace ClipNote {
namespace UpdateInstaller {
namespace {

// 临时目录都叫这个名字打头，cleanStaleWorkDirs() 靠它认领自己的垃圾
const QString kWorkDirPrefix = QStringLiteral("ClipNoteUpdate-");

// 文件开头是不是 "MZ"（PE 可执行文件）。
// 拿这个挡的是「下载到的其实是个 404 页面/HTML 错误页」那类情况 ——
// 大小和 sha256 都校验过了也挡不住「Release 里挂错了附件」。
bool looksLikeExecutable(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    return file.read(2) == QByteArrayLiteral("MZ");
}

// 在暂存目录里找主程序。优先按目标 exe 的原名找，找不到就退而求其次：
// 只在一个 .exe 的时候用它。两个以上又不叫那个名字，就宁可报错也不要瞎猜 ——
// 猜错了就是把一个不相干的程序装到用户机器上。
QString findStagedExe(const QString &stageDir, const QString &preferredName)
{
    QStringList named;
    QStringList others;

    QDirIterator it(stageDir, {QStringLiteral("*.exe")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        // 只认两层的：zip 允许把内容放在一个顶层文件夹里，再深就是解压意外了
        const int depth = QDir(stageDir).relativeFilePath(path).count(QLatin1Char('/'));
        if (depth > 1)
            continue;

        if (QFileInfo(path).fileName().compare(preferredName, Qt::CaseInsensitive) == 0)
            named.append(path);
        else
            others.append(path);
    }

    if (!named.isEmpty())
        return named.first();
    if (others.size() == 1)
        return others.first();
    return QString();
}

} // namespace

QString workDirFor(const QString &version)
{
    return QDir::temp().filePath(kWorkDirPrefix + version);
}

QString systemToolPath(const QString &relative)
{
#ifdef Q_OS_WIN
    wchar_t buffer[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        const QString candidate =
            QDir(QString::fromWCharArray(buffer, static_cast<int>(length))).filePath(relative);
        if (QFileInfo::exists(candidate))
            return candidate;
    }
#endif
    // 系统目录里没有就退回裸名字，交给系统去搜 PATH。
    // 正常路径不该走到这儿 —— System32 下这两个都有。
    return relative;
}


bool stage(const QString &downloadedFile, const QString &version,
           Prepared *out, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    if (!QFileInfo::exists(downloadedFile))
        return fail(QStringLiteral("下载下来的文件不见了：%1").arg(downloadedFile));

    const QString workDir = workDirFor(version);
    const QString stageDir = QDir(workDir).filePath(QStringLiteral("stage"));

    // 同一个版本重试时，上一轮的暂存内容要清掉。
    //
    // **只清 stage 子目录，不能清整个 workDir**：下载下来的那个文件也在
    // workDir 里面（调用方把它放在那儿），把 workDir 整个删掉就等于在解压之前
    // 先把源文件删了。自检里一度没发现这个，就是因为测试用的下载文件放在别处。
    QDir(stageDir).removeRecursively();
    if (!QDir().mkpath(stageDir))
        return fail(QStringLiteral("建不出暂存目录：%1").arg(stageDir));

    const QString fileName = QFileInfo(downloadedFile).fileName();

    if (fileName.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) {
        // bsdtar 按内容识别格式，所以 -xf 对 zip 一样管用。
        // 用系统自带的，不引 libzip/quazip —— 一个解压功能不值得多一个依赖。
        //
        // 走绝对路径而不是裸的 "tar"：PATH 上排在前面的可能是 Git for Windows 或
        // Cygwin 带的 **GNU tar**，那个根本不解 zip（实测它会把 zip 当 tar 试着读，
        // 直接报错）。顺带也躲开了「PATH 上有个同名恶意程序」这种局面。
        QProcess tar;
        tar.setProgram(systemToolPath(QStringLiteral("tar.exe")));
        tar.setArguments({QStringLiteral("-xf"), QFileInfo(downloadedFile).absoluteFilePath(),
                          QStringLiteral("-C"), stageDir});
        tar.start();
        if (!tar.waitForStarted(5000)) {
            return fail(QStringLiteral("启动不了解压程序 %1。这个功能要用 Windows 10 1803 起"
                                       "自带的 tar.exe，更老的系统上请手动更新。")
                            .arg(tar.program()));
        }
        if (!tar.waitForFinished(120000)) {
            tar.kill();
            return fail(QStringLiteral("解压超时（120 秒）"));
        }
        if (tar.exitStatus() != QProcess::NormalExit || tar.exitCode() != 0) {
            return fail(QStringLiteral("解压失败（tar 退出码 %1）：%2")
                            .arg(tar.exitCode())
                            .arg(QString::fromLocal8Bit(tar.readAllStandardError()).trimmed()));
        }
    } else {
        const QString dest = QDir(stageDir).filePath(fileName);
        if (!QFile::copy(downloadedFile, dest))
            return fail(QStringLiteral("把下载到的文件复制到暂存目录失败"));
    }

    const QString preferred = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
    const QString stagedExe = findStagedExe(stageDir, preferred);

    if (stagedExe.isEmpty()) {
        return fail(QStringLiteral("压缩包里没找到主程序（找的是 %1，或者唯一的一个 .exe）")
                        .arg(preferred));
    }
    if (!looksLikeExecutable(stagedExe))
        return fail(QStringLiteral("找到的 %1 不是可执行文件，这个包多半是坏的")
                        .arg(QFileInfo(stagedExe).fileName()));

    Prepared prepared;
    prepared.workDir = workDir;
    prepared.stageDir = QFileInfo(stagedExe).absolutePath();
    prepared.targetExe = QCoreApplication::applicationFilePath();

    if (out)
        *out = prepared;
    return true;
}

bool canReplace(const QString &exePath, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    const QFileInfo info(exePath);
    const QDir dir = info.absoluteDir();

    // 直接在安装目录里试建一个文件。除此之外没有别的可靠办法 ——
    // 光看权限位在 Windows 上不准，而「能不能重命名正在运行的 exe」这个更准的测试
    // 恰好因为程序正在运行而必然失败。
    QTemporaryFile probe(dir.filePath(QStringLiteral("clipnote-write-probe-XXXXXX")));
    if (!probe.open())
        return fail(QStringLiteral("装 ClipNote 的这个目录写不进去：%1\n"
                                   "（装在 Program Files 之类的地方时会出现这种情况，"
                                   "更新得手动来）").arg(dir.absolutePath()));

    return true;
}

bool launchSwap(const Prepared &prepared, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    QFile scriptFile(QStringLiteral(":/apply-update.ps1"));
    if (!scriptFile.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("读不到内置的更新脚本（资源 apply-update.ps1 没编进来？）"));

    QString script = QString::fromUtf8(scriptFile.readAll());

    // 路径进的是 PowerShell 的单引号字符串，所以只需要把单引号自身翻倍
    const auto quote = [](QString text) {
        text.replace(QLatin1Char('\''), QLatin1String("''"));
        return text;
    };

    script.replace(QLatin1String("@@EXE@@"), quote(prepared.targetExe));
    script.replace(QLatin1String("@@STAGE@@"), quote(prepared.stageDir));
    script.replace(QLatin1String("@@WORK@@"), quote(prepared.workDir));
    script.replace(QLatin1String("@@PID@@"), QString::number(QCoreApplication::applicationPid()));

    // -EncodedCommand 要的是 UTF-16LE 再 Base64。
    // 走这条路就完全绕开了控制台代码页，中文路径不会烂掉 —— 这正是这套东西
    // 用 PowerShell 而不是批处理的原因。
    const QString utf16 = script;
    const QByteArray raw(reinterpret_cast<const char *>(utf16.utf16()), utf16.size() * 2);
    const QString encoded = QString::fromLatin1(raw.toBase64());

    QProcess process;
    // 同样走绝对路径，理由和上面的 tar 一样
    process.setProgram(systemToolPath(QStringLiteral("WindowsPowerShell/v1.0/powershell.exe")));
    process.setArguments({QStringLiteral("-NoProfile"),
                          QStringLiteral("-NonInteractive"),
                          QStringLiteral("-EncodedCommand"), encoded});

    // 没这一下会闪一个黑窗口出来。GUI 程序里跑控制台程序都会这样。
    // 注意**没有** -ExecutionPolicy Bypass：-EncodedCommand 传的是命令而不是 .ps1 文件，
    // 执行策略本来就不拦它，不需要去动那个开关。
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
#endif

    if (!process.startDetached()) {
        return fail(QStringLiteral("启动更新脚本失败。可能是系统里没有 powershell.exe。"));
    }

    return true;
}

void cleanStaleWorkDirs()
{
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-7);

    const QDir temp = QDir::temp();
    const QFileInfoList entries = temp.entryInfoList(
        {kWorkDirPrefix + QStringLiteral("*")}, QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QFileInfo &entry : entries) {
        // 只动七天前的。正在进行的更新会一直改这些文件，时间戳是新的。
        if (entry.lastModified() < cutoff)
            QDir(entry.absoluteFilePath()).removeRecursively();
    }
}

} // namespace UpdateInstaller
} // namespace ClipNote
