; ClipNote 安装程序（Inno Setup 脚本）
;
; **不要直接双击编译**，用 tools/build-installer.ps1 —— 版本号和要打包的文件目录
; 都是命令行传进来的（见下面的 #ifndef 兜底），直接编译会用兜底值，
; 打出一个版本号写着 0.0.0、内容是空的安装包。
;
; 用到的 Inno Setup 特性里，有两条是这个项目非有不可的：
;   1. [Dirs] 的 Permissions: users-modify —— 见那一段的注释，它是「装到
;      Program Files 之后还能自更新」的全部秘密
;   2. [Code] 里的 taskkill —— ClipNote 是托盘程序，会忽略关闭请求

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\build\installer\staging"
#endif
#ifndef OutputDir
  #define OutputDir "..\build\installer"
#endif

; 对外的版本号带 v，和 GitHub 的 tag 保持一致，也用在产物文件名上。
;
; 注意 [Setup] 里的 AppVersion 和 VersionInfoVersion **必须保持纯数字**
; （VersionInfoVersion 只吃 n.n.n.n），所以那两个用 AppVersion，
; 人眼看到的地方（文件名、AppVerName）才用带 v 的这个。
#define AppVersionTag "v" + AppVersion

#define AppName "ClipNote"
#define AppExeName "ClipNote.exe"
#define AppPublisher "ihehe1996"
#define AppUrl "https://github.com/ihehe1996/ClipNote"

[Setup]
; AppId 是这个程序在 Windows 眼里的身份。**一旦发出去就不要再改** ——
; 改了之后新版本会被当成另一个软件，覆盖安装变成并存两份、卸载项也会有两个。
AppId={{A4BD1670-7CE8-40BC-88C5-2FAA807D5DCE}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersionTag}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}
AppUpdatesURL={#AppUrl}
VersionInfoVersion={#AppVersion}

; {autopf} 在 64 位系统上就是 C:\Program Files
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
DefaultGroupName={#AppName}

; 装到 Program Files 需要管理员，这一步的 UAC 是躲不掉的，也只能弹这一次。
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

OutputDir={#OutputDir}
; 名字里带 install 也是给更新器看的：pickUpdateAsset() 会跳过 setup / install，
; 免得它把安装程序当成主程序换上去。改这里要连着看 tools/build-installer.ps1。
OutputBaseFilename={#AppName}-{#AppVersionTag}-install
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#AppExeName}
UninstallDisplayName={#AppName}
SetupLogging=yes

; 安装程序自己也带 logo。不加这行 Setup.exe 显示的是 Windows 的默认图标。
; 相对路径按本 .iss 文件所在目录解析，所以是 ..\resources。
; 这份 ico 是 tools/make-icon.py 从 resources/clipnote.png 生成的，
; 和编进 exe 资源段的是**同一个文件**（见 resources/clipnote.rc）。
SetupIconFile=..\resources\clipnote.ico

; 关闭「友好关闭正在运行的程序」：见 [Code] 里的说明，对 ClipNote 它不管用，
; 留着只会多弹一个关不掉的提示框。
CloseApplications=no
RestartApplications=no

; 关掉「用了 HKCU 但 PrivilegesRequired=admin」这个警告。下面 [Registry] 那段有完整
; 说明；简单说：正常装法（自己是管理员、点一下 UAC 确认）提权不换用户，
; HKCU 就是当前用户自己的。它对不上的只有「管理员替别人输密码装」
; （over-the-shoulder）那一种，那种场合确实会把自启项写到管理员名下 ——
; 个人工具基本遇不到，而换成 HKLM 反而更糟（普通用户就没法关掉自启动了）。
UsedUserAreasWarning=no

[Languages]
Name: "chinese"; MessagesFile: "ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
chinese.ExtraGroup=其他选项：
english.ExtraGroup=Additional options:
chinese.AutostartTask=开机时自动启动 {#AppName}（只驻留托盘，不弹出面板）
english.AutostartTask=Start {#AppName} when Windows starts (tray only, no panel)
chinese.LaunchApp=立即运行 {#AppName}
english.LaunchApp=Launch {#AppName}

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:ExtraGroup}"; Flags: unchecked
Name: "autostart"; Description: "{cm:AutostartTask}"; GroupDescription: "{cm:ExtraGroup}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Registry]
; 开机自启写在 HKCU 而不是 HKLM：
;   - 「要不要开机启动」是每个用户自己的事。写进 HKLM 的话，同一台机器上不是
;     管理员的用户**想关都关不掉**（改 HKLM 要提权，任务管理器的启用/停用
;     开关也写不进去）——那是替用户做主的做法
;   - 写在 HKCU 里，任务管理器的「启动」页能直接看到、能停用
;
; --silent 让它开机时只驻留托盘、不弹面板，见 src/AppConfig.h 的
; isSilentStartRequested()。没有这个参数的话每次开机都会在屏幕中间弹个面板，
; 那是打扰不是功能。
;
; 提权与 HKCU 的关系：UAC 提权**不改变用户身份**，只是把令牌换成同一个用户的
; 高完整性版本，所以这里写进去的还是当前用户自己的 HKCU。只有
; 「管理员替别人输密码安装」那种情况会错位，见 [Setup] 里 UsedUserAreasWarning
; 那段的说明。
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
    ValueType: string; ValueName: "{#AppName}"; ValueData: """{app}\{#AppExeName}"" --silent"; \
    Flags: uninsdeletevalue; Tasks: autostart

[Dirs]
; ★ 这是整个安装程序里最关键的一行 ★
;
; 把安装目录的写权限授予本机所有用户。装的时候弹的这一次 UAC 本来就要弹，
; 而有了它，之后的自更新（ClipNote 直接替换自己的 exe 和依赖 DLL）就只是
; 普通文件写入，**再也不用提权**。没有它，每次更新都会卡在
; UpdateInstaller::canReplace() 上，退化成「打开下载页」手动换。
;
; 代价说清楚：本机任何用户都能改这个目录里的程序文件。对一个以普通权限运行、
; 没有任何系统级状态的托盘工具来说，这点让步换来的是「更新不打扰用户」。
Name: "{app}"; Permissions: users-modify

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchApp}"; Flags: nowait postinstall skipifsilent

[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
begin
  // ClipNote 是托盘程序：它的 MainWindow::closeEvent 会 event->ignore()，
  // 点 ✕ 只是收起面板而不是退出（见 src/ui/MainWindow.cpp 的 closeEvent）。
  // 于是 Inno 内置的「友好关闭占用文件的程序」对它永远失败，覆盖安装时
  // 必然弹一个「安装程序无法自动关闭所有应用程序」——用户点了也过不去。
  //
  // 这里直接结束掉。剪贴板历史本来就只存在内存里、退出即丢，没有额外损失。
  Exec('taskkill.exe', '/F /IM {#AppExeName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Result := '';
end;
