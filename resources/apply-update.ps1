# ClipNote 自更新的收尾脚本（**模板**，不是拿来直接执行的）
#
# 为什么要有这么一个东西：Windows 上没法替换掉一个正在运行的 exe（文件被锁住），
# 所以程序只能先把自己下载好的新版本放到一边，启动一个「送葬人」，然后退出。
# 送葬人等旧进程消失，把文件换掉，再把新版本启动起来。
#
# 为什么是 PowerShell 而不是批处理：**中文路径**。.bat 的内容要经过控制台代码页解释，
# 路径里带非 ASCII 就乱码；而 ClipNote 是用 `powershell -EncodedCommand` 启动它的，
# 传进去的是 UTF-16LE + Base64 的完整字符串，完全不经过代码页。
#
# 占位符由 src/update/UpdateInstaller.cpp 替换（tools/test-updater.py 用同样的名字
# 做同样的替换，这样两边测的是同一份脚本）：
#   @@PID@@   要等的进程号
#   @@EXE@@   要被替换掉的 exe 全路径
#   @@STAGE@@ 暂存目录，新版本的主程序（和它带的依赖 DLL）都在里面
#   @@WORK@@  本次更新的临时目录
#
# 调用方负责把路径里的单引号翻倍 —— PowerShell 的单引号字符串用 '' 转义。
#
# 这个脚本有一条总的约束：**任何一步失败，都要把机器还原成没更新过的样子**。
# 半途而废的结果不是「更新没成功」，而是「用户手上只剩一个双击没反应的图标」。
# 所以下面的结构是：所有会出错的事都塞进一个 try，只用一个标志位判断成败，
# 失败统一走同一段回滚逻辑 —— 分开写 catch 一定会漏掉某条路径。

$ErrorActionPreference = 'Stop'

$exe   = '@@EXE@@'
$stage = '@@STAGE@@'
$work  = '@@WORK@@'
$targetPid = @@PID@@

$log        = Join-Path $work 'apply-update.log'
$installDir = Split-Path -Parent $exe
$backupExe  = "$exe.old"
$backupDir  = Join-Path $work 'backup'

function Write-Log($message)
{
    "$([DateTime]::Now.ToString('yyyy-MM-dd HH:mm:ss'))  $message" |
        Out-File -FilePath $log -Append -Encoding utf8
}

# 为回滚记下：覆盖前文件在哪、原先存不存在
$overwritten = @()
$replaced    = $false   # 旧 exe 是否已经被挪到 $backupExe
$succeeded   = $false

function Restore-Overwritten
{
    foreach ($entry in $overwritten) {
        if ($entry.Existed) {
            Copy-Item -LiteralPath $entry.Backup -Destination $entry.Target -Force -ErrorAction SilentlyContinue
        } else {
            Remove-Item -LiteralPath $entry.Target -Force -ErrorAction SilentlyContinue
        }
    }
}

try {
    Write-Log "开始更新：$exe <- $stage"

    # ---- 1. 等旧进程退出 ----
    # 上限 60 秒。到点了还没退也不要紧，下一步会真正判定文件锁。
    $deadline = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $deadline) {
        if (-not (Get-Process -Id $targetPid -ErrorAction SilentlyContinue)) { break }
        Start-Sleep -Milliseconds 300
    }
    Write-Log "旧进程已退出（或等待超时）"

    # ---- 2. 把旧 exe 挪开 ----
    # 这一步同时干两件事：**测试文件是不是真的不锁了**，以及留个后路。
    # 正在运行的 exe 在 Windows 上没法被重命名，所以挪得动 = 可以换了。
    # 光等进程列表消失是不够的：进程退出和句柄释放之间有一小段窗口。
    Remove-Item -LiteralPath $backupExe -Force -ErrorAction SilentlyContinue

    $moved = $false
    for ($i = 0; $i -lt 100; $i++) {
        try {
            Move-Item -LiteralPath $exe -Destination $backupExe -Force -ErrorAction Stop
            $moved = $true
            break
        } catch {
            Start-Sleep -Milliseconds 300
        }
    }
    if (-not $moved) {
        throw "旧程序一直不让出文件（等了 30 秒）"
    }
    $replaced = $true
    Write-Log "旧程序已备份到 $backupExe"

    # ---- 3. 备份所有即将被覆盖的文件 ----
    # 不只是 exe：包里还带着依赖 DLL，它们要是坏的，光把 exe 回滚回去也救不回来。
    if (Test-Path -LiteralPath $stage) {
        foreach ($src in (Get-ChildItem -LiteralPath $stage -Recurse -File)) {
            $relative = $src.FullName.Substring($stage.Length).TrimStart('\')
            $target   = Join-Path $installDir $relative
            $backup   = Join-Path $backupDir $relative

            $existed = Test-Path -LiteralPath $target
            if ($existed) {
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $backup) | Out-Null
                Copy-Item -LiteralPath $target -Destination $backup -Force
            }
            $overwritten += [pscustomobject]@{
                Target = $target; Backup = $backup; Existed = $existed
            }
        }
        Write-Log "备份了 $($overwritten.Count) 个将被覆盖的文件"
    }

    # ---- 4. 覆盖 ----
    Copy-Item -Path (Join-Path $stage '*') -Destination $installDir -Recurse -Force
    if (-not (Test-Path -LiteralPath $exe)) {
        throw "覆盖之后主程序不在原来的位置了"
    }
    Write-Log "覆盖完成"

    # ---- 5. 启动新版本，确认它真的活着 ----
    # 「文件换好了」不等于「程序能用」。新版本要是起来就崩（少了 DLL、包坏了），
    # 这里必须能看出来 —— 否则用户手上就只剩一个点了没反应的图标，而且他很难
    # 想到问题出在自动更新上。Start-Process 自己抛异常也要算失败。
    $started = $null
    try {
        $started = Start-Process -FilePath $exe -WorkingDirectory $installDir -PassThru
    } catch {
        throw "启动新版本失败：$_"
    }

    Start-Sleep -Seconds 4
    if ($started.HasExited) {
        throw "新版本启动后立刻退出了（退出码 $($started.ExitCode)）"
    }

    $succeeded = $true
    Write-Log "更新成功，新版本已在运行"
}
catch {
    Write-Log "失败：$_"
}

if (-not $succeeded) {
    Write-Log "开始回滚"

    Restore-Overwritten

    if ($replaced) {
        Remove-Item -LiteralPath $exe -Force -ErrorAction SilentlyContinue
        Move-Item -LiteralPath $backupExe -Destination $exe -Force -ErrorAction SilentlyContinue
        try {
            Start-Process -FilePath $exe -WorkingDirectory $installDir
            Write-Log "已还原成原来那一版并重新启动"
        } catch {
            Write-Log "还原之后启动也失败了：$_"
        }
    }

    # 失败时**留着**整个临时目录：里面有日志和备份，是排查的唯一线索
    exit 1
}

# 成功才走到这里
Remove-Item -LiteralPath $backupExe -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $backupDir -Recurse -Force -ErrorAction SilentlyContinue

# 留 2 秒再删自己所在的目录，免得新进程还在读里面的东西
Start-Sleep -Seconds 2
Set-Location $env:TEMP
Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
exit 0
