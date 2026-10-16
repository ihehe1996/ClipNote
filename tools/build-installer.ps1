<#
.SYNOPSIS
    构建 ClipNote 的两个发布产物：安装程序和便携 zip。

.DESCRIPTION
    一次跑完：编译 release → windeployqt 铺依赖 → Inno Setup 打安装包 → 压便携 zip。

    为什么要两个：
      · 安装程序给「想装到 Program Files、进开始菜单、有卸载项」的人
      · 便携 zip 给「解压到哪就在哪跑」的人，**而且自更新器要的就是它** ——
        GitHub Release 上两个都挂着时，ClipNote 会挑 zip 而不是安装程序
        （见 UpdateChecker 的 pickUpdateAsset，挑错了会把主程序换成安装程序）

    版本号来自 build/<preset>/version.txt，那是 CMake 在 configure 时从
    CMakeLists.txt 的 project(... VERSION ...) 写出来的 —— 全项目只有那一处。

.EXAMPLE
    powershell -File tools\build-installer.ps1
    powershell -File tools\build-installer.ps1 -SkipBuild

.NOTES
    如果你的执行策略是 Restricted（本机默认 RemoteSigned，能直接跑），
    别用 -ExecutionPolicy Bypass 去绕 —— 那个开关是把脚本执行的防护整个关掉。
    该改的是策略本身：Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
#>
[CmdletBinding()]
param(
    # 已经编好了就不重复编（CI 或连着改打包脚本时有用）
    [switch]$SkipBuild,
    [string]$Preset = "mingw-release"
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot "build\release"
$StagingDir = Join-Path $RepoRoot "build\installer\staging"
$OutputDir = Join-Path $RepoRoot "build\installer"
$IssFile = Join-Path $RepoRoot "installer\ClipNote.iss"

function Fail($message) {
    Write-Host "错误：$message" -ForegroundColor Red
    exit 1
}

# ---- 1. 找 Inno Setup 的命令行编译器 ----
# 从 CMakeCache 里问 Qt 装在哪，比再写死一遍路径强 ——
# 那个路径在 CMakePresets / .vscode 里已经写了好几处了。
function Find-Iscc {
    $candidates = @(
        (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"),
        "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
        "C:\Program Files\Inno Setup 6\ISCC.exe"
    )
    foreach ($path in $candidates) {
        if (Test-Path -LiteralPath $path) { return $path }
    }
    $onPath = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    return $null
}

function Find-MingwBin {
    # MinGW 的 bin 目录。windeployqt 的 --compiler-runtime 要先找到
    # libgcc_s_seh-1.dll / libstdc++-6.dll / libwinpthread-1.dll 才能拷进来；
    # 找不到它只会在 PATH 里瞎找、并且**只给一句警告**，打出来的包在别人
    # 机器上双击就是「找不到 xxx.dll」。从 CMakeCache 里问编译器在哪最靠谱。
    $cache = Join-Path $BuildDir "CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $cache)) { return $null }

    # 类型这一段是 UNINITIALIZED 而不是 FILEPATH（这是 CMake 自己设的编译器变量，
    # 不走 TARGET 那套类型推断），所以别写死 :FILEPATH= —— 写死了会静默匹配不上，
    # 然后退回「靠 PATH 碰运气」，还只在打出来的包上出错。
    $match = Select-String -Path $cache -Pattern '^CMAKE_CXX_COMPILER:[A-Z]+=(.+)$' | Select-Object -First 1
    if (-not $match) { return $null }

    $dir = Split-Path -Parent $match.Matches[0].Groups[1].Value.Trim()
    if (Test-Path -LiteralPath (Join-Path $dir "libgcc_s_seh-1.dll")) { return $dir }
    return $null
}

function Find-QtBin {
    $cache = Join-Path $BuildDir "CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $cache)) { return $null }

    $match = Select-String -Path $cache -Pattern '^Qt6_DIR:PATH=(.+)$' | Select-Object -First 1
    if (-not $match) { return $null }

    # <qt>/lib/cmake/Qt6 -> <qt>
    $qtDir = $match.Matches[0].Groups[1].Value.Trim()
    $qtRoot = Split-Path (Split-Path (Split-Path $qtDir -Parent) -Parent) -Parent
    $bin = Join-Path $qtRoot "bin"
    if (Test-Path -LiteralPath (Join-Path $bin "windeployqt.exe")) { return $bin }
    return $null
}

$Iscc = Find-Iscc
if (-not $Iscc) {
    Fail @"
找不到 ISCC.exe（Inno Setup 的命令行编译器）。
装一下：winget install JRSoftware.InnoSetup
"@
}

# ---- 2. 编译 ----
if (-not $SkipBuild) {
    Write-Host "构建 $Preset ..." -ForegroundColor Cyan
    & cmake --build --preset $Preset
    if ($LASTEXITCODE -ne 0) { Fail "构建失败" }
}

$exe = Join-Path $BuildDir "ClipNote.exe"
if (-not (Test-Path -LiteralPath $exe)) { Fail "没找到 $exe，先跑一次构建" }

# ---- 3. 版本号（CMake 是唯一来源）----
$versionFile = Join-Path $BuildDir "version.txt"
if (-not (Test-Path -LiteralPath $versionFile)) {
    Fail "没找到 version.txt。它由 CMakeLists.txt 的 configure 阶段生成，重新 configure 一次。"
}
$version = (Get-Content -LiteralPath $versionFile -Raw).Trim()
if ($version -notmatch '^\d+\.\d+') { Fail "version.txt 里的版本号不正常：'$version'" }
Write-Host "版本 $version" -ForegroundColor Cyan

# ---- 4. 铺依赖到暂存目录 ----
$QtBin = Find-QtBin
if (-not $QtBin) {
    Fail "从 CMakeCache 里问不出 Qt 的 bin 目录，或者那里没有 windeployqt.exe"
}

# 把 MinGW 的 bin 放进 PATH，让 --compiler-runtime 一定能找到那三个运行时 DLL
$MingwBin = Find-MingwBin
if ($MingwBin) {
    $env:PATH = "$MingwBin;$env:PATH"
} else {
    Write-Host "  警告：没问出 MinGW 的 bin 目录，运行时 DLL 要靠 PATH 碰运气" -ForegroundColor Yellow
}

Write-Host "铺 Qt 依赖 ..." -ForegroundColor Cyan
if (Test-Path -LiteralPath $StagingDir) { Remove-Item -LiteralPath $StagingDir -Recurse -Force }
New-Item -ItemType Directory -Path $StagingDir -Force | Out-Null
Copy-Item -LiteralPath $exe -Destination $StagingDir

# 注意 windeployqt 一定会打一句
#   Warning: Runtime libraries not found in Qt binary folder, defaulting to looking in path
# 这句**消不掉、也不用管**：Qt 自己的 bin 目录里本来就不放 MinGW 的运行时，
# 它只能去 PATH 里找 —— 上面把 MinGW 的 bin 塞进 PATH 就是为了让这个回退找得着。
# 真正要保证的是「它找着了」，那个由下面两道检查兜底。
#
# 开关的取舍：
#   --compiler-runtime  不能省。MinGW 的 libgcc/libstdc++/libwinpthread 不在
#                       Windows 里，少了它们别人机器上双击就是「找不到 xxx.dll」
#   --no-opengl-sw      省掉 20MB 的 opengl32sw.dll，纯 QWidget 程序用不到。
#                       万一以后有机器起不来，先怀疑这一条
#   --no-system-d3d-compiler / --no-system-dxc-compiler
#                       同理，那是给 Qt Quick 的 D3D 后端用的，这里没用到。
#                       不加 dxc 那条的话 windeployqt 会抱怨找不到 dxcompiler.dll
& (Join-Path $QtBin "windeployqt.exe") `
    --release --no-translations --no-opengl-sw --compiler-runtime `
    --no-system-d3d-compiler --no-system-dxc-compiler `
    (Join-Path $StagingDir "ClipNote.exe")
if ($LASTEXITCODE -ne 0) { Fail "windeployqt 失败" }

# 光看 windeployqt 的返回值不够：它漏拷运行时 DLL 时也只是警告一句。
# 这三个文件缺了，包在**别人**机器上就起不来 —— 自己的机器上因为有 PATH 反而正常，
# 是最容易漏过去的那类错。
foreach ($dll in @("libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll",
                   "Qt6Core.dll", "Qt6Widgets.dll", "Qt6Network.dll")) {
    if (-not (Test-Path -LiteralPath (Join-Path $StagingDir $dll))) {
        Fail "暂存目录里缺 $dll —— 这个包在别人机器上会起不来"
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $StagingDir "platforms\qwindows.dll"))) {
    Fail "暂存目录里缺 platforms\qwindows.dll —— 缺了它程序根本起不来"
}

# 文件名都在还不够，得真的跑起来。**关键是给一个不含 Qt 的最小 PATH** ——
# 开发机上 Qt 的 bin 一直在 PATH 里，就算依赖一个没拷也能跑，这类错只有
# 换台机器才暴露；把 PATH 剥干净就当场露馅了。
# 用 --selftest 而不是正常启动：它无界面、跑完就退、还顺带验了平台插件和托盘。
Write-Host "验证包能独立运行 ..." -ForegroundColor Cyan
$probeDir = Join-Path $OutputDir "deploy-check"
if (Test-Path -LiteralPath $probeDir) { Remove-Item -LiteralPath $probeDir -Recurse -Force }
New-Item -ItemType Directory -Path $probeDir -Force | Out-Null
$savedPath = $env:PATH
try {
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $probe = Start-Process -FilePath (Join-Path $StagingDir "ClipNote.exe") `
                           -ArgumentList "--selftest" `
                           -WorkingDirectory $probeDir -Wait -PassThru -NoNewWindow
    if ($probe.ExitCode -ne 0) {
        Fail "脱离 Qt 环境跑自检失败（退出码 $($probe.ExitCode)）。多半是依赖没拷全，看 $probeDir\selftest.log"
    }
} finally {
    $env:PATH = $savedPath
    Remove-Item -LiteralPath $probeDir -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "  能独立运行（无 Qt 的 PATH 下自检通过）" -ForegroundColor DarkGray

$staged = (Get-ChildItem -LiteralPath $StagingDir -Recurse -File | Measure-Object).Count
Write-Host "  暂存目录 $staged 个文件" -ForegroundColor DarkGray

# ---- 5. 打安装程序 ----
# 先把上一轮的产物清掉（包括改名之前留下的旧命名）。留着的话发 Release 时
# 很容易把上一版的文件拖上去 —— 两个文件名只差一个数字，看走眼太正常了。
if (Test-Path -LiteralPath $OutputDir) {
    Get-ChildItem -LiteralPath $OutputDir -File |
        # setup 是改名前的旧后缀，留着是为了把更早那几轮的产物也清掉
        Where-Object { $_.Name -match '^ClipNote-.*(-(setup|install)\.exe|\.zip)$' } |
        Remove-Item -Force
}

Write-Host "打安装程序 ..." -ForegroundColor Cyan
& $Iscc "/DAppVersion=$version" "/DSourceDir=$StagingDir" "/DOutputDir=$OutputDir" $IssFile
if ($LASTEXITCODE -ne 0) { Fail "Inno Setup 编译失败" }

# ---- 6. 压便携 zip ----
# 自更新器优先挑 zip（它能整包带上依赖 DLL），所以这个不是可选项
Write-Host "压便携 zip ..." -ForegroundColor Cyan
$zipPath = Join-Path $OutputDir "ClipNote-v$version.zip"
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
Compress-Archive -Path (Join-Path $StagingDir "*") -DestinationPath $zipPath -CompressionLevel Optimal

# ---- 7. 汇报 ----
Write-Host ""
Write-Host "产物：" -ForegroundColor Green
foreach ($artifact in @((Join-Path $OutputDir "ClipNote-v$version-install.exe"), $zipPath)) {
    if (Test-Path -LiteralPath $artifact) {
        $mb = [math]::Round((Get-Item -LiteralPath $artifact).Length / 1MB, 1)
        Write-Host ("  {0}  ({1} MB)" -f $artifact, $mb)
    }
}
Write-Host ""
Write-Host "发 Release 时两个都传成附件，tag 写 v$version。" -ForegroundColor DarkGray
Write-Host "更新器会挑 zip；名字里带 setup/install 的它一律跳过。" -ForegroundColor DarkGray
