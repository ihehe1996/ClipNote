# ClipNote

剪贴板笔记工具。Qt 6 Widgets + Windows 桌面，形态是**托盘常驻**：启动时把面板显示出来，
收起后只留一个托盘图标，之后按全局热键（默认 `Alt+Q`）唤出。关掉面板只是收起来，
退出走托盘右键菜单。

已实现的功能：

- **剪贴板历史**——后台自动收集你复制过的**文本和图片**，最新的排在最前，重复的内容
  不会存两份（会挪到最前），上限 200 条，超出从最旧的丢。
  每行左边是类型区（文本是段落字形、图片是缩略图），中间内容（太长会省略），
  右边一个灰色的相对时间（「刚刚」「5 分钟前」…）
- **不抢焦点的面板**——按热键唤出面板时，你正在打字的那个程序**连光标都不会闪**，
  可以接着打字。做法和系统自带的剪贴板（Win+V）一样
- **单击即粘贴**——鼠标点一条，内容直接落在你原来那个程序的光标处，面板**不会关闭**，
  可以连着点好几条；点选**不会打乱列表顺序**
- **右键管理**——右键条目可删除单条，右键空白处可清空全部历史
- **自绘标题栏**——面板顶部是自绘的（系统标题栏加不了自定义按钮，见下面说明），
  左边是图标和标题，右边是**齿轮菜单**（检查更新 / 关于）和**关闭**按钮，
  空白处可以拖动窗口
- **检查更新**——从 GitHub Releases 查版本，有新版本会下载下来自己替换自己
  （见下面「更新是怎么做的」）

> 历史只存在内存里，退出就没了；图片也一样（所以复制一张特别大的图会被直接跳过，
> 见 `ClipStore::kMaxImagePixels`）。文件、钉住、落盘持久化都还没做。

## 怎么用

1. 在任意程序里正常复制东西（`Ctrl+C`），ClipNote 在后台收着
2. 按 `Alt+Q`（全局热键，任何程序里都管用）唤出面板 —— 程序刚启动时面板已经开着，
   不用按键
3. **单击**列表里的某一条
4. 内容当场粘回你原来那个窗口的光标处，面板留着不关，可以接着点下一条

**右键**列表里的某一条可以「删除这一条」，点在空白处则是「清空全部历史」。

点条目**不会改变列表顺序** —— 你选的那条留在原位。面板不关闭，所以连着点好几条时
每次都落在你眼睛盯的那一条上，不会错位。

收起面板的方式：再按一次 `Alt+Q`、点标题栏右边的 ✕，或者走托盘菜单。

> **点面板外面不会把它收起来** —— 这是刻意的。面板不抢焦点，所以你点自己那个程序
> （比如想把光标挪到要粘贴的位置）是完全正常的操作，之前那版会因此把面板弄没。

**面板永远是置顶的** —— 它不会被你其它窗口盖住。这是刻意的：面板不抢焦点，
所以普通窗口那些「被盖住了点一下就浮上来」的直觉操作它一概没有，与其给一堆开关
让你自己找回来，不如干脆不让它被盖住。

标题栏空白处可以拖动窗口。标题栏右边的**齿轮**里有两项：**检查更新**和**关于 ClipNote**。

## 更新是怎么做的

程序会从 GitHub Releases 查版本（仓库地址配在 `src/AppConfig.h` 的 `updateRepo()` 里），
有两种触发方式：

- **启动时悄悄查一次**——查到新版本只弹个托盘气泡，点它才打开更新窗口。
  有节流（默认 12 小时一次），不会每次开程序都去敲 GitHub
- **齿轮菜单里的「检查更新」**——随时手点，不受节流限制

发现新版本后，点「下载并更新」会：

1. 下载附件，**校验大小和 sha256**（GitHub 给每个附件都记了摘要）
2. 解开 `.zip`（或直接用单个 `.exe`），确认里面那个主程序真的是个可执行文件
3. 启动一个小脚本，然后 **ClipNote 自己退出** —— Windows 上没法替换正在运行的 exe，
   只能让另一个进程等它走了再动手
4. 脚本等文件不再被占用 → 备份旧的 → 覆盖 → 启动新版 → **确认它真的活着**
5. 都成功了就删掉备份；**任何一步失败，整套还原回去并把旧的重新启动**

更新源配在 [src/AppConfig.h](src/AppConfig.h) 的 `updateRepo()` 里，
格式是 GitHub 的「用户名/仓库名」（当前是 `ihehe1996/ClipNote`），不需要自己搭服务器。

## 安装

有两种给别人的方式，[tools/build-installer.ps1](tools/build-installer.ps1) 一次把两个都产出：

```bash
powershell -File tools/build-installer.ps1
```

| 产物 | 适合 |
|---|---|
| `ClipNote-v1.0.0-setup.exe` | 装到 `Program Files`，有开始菜单项、卸载项，安装时可以勾桌面快捷方式和开机自启 |
| `ClipNote-v1.0.0.zip` | 解压到任意目录就能跑，不写注册表、不留痕 |

装完之后**自动更新照样能用**：安装程序会把安装目录的写权限授予用户，
所以 ClipNote 换自己的文件时不需要再弹 UAC。装的时候那**一次** UAC 是躲不掉的
（写 `Program Files` 本来就要管理员权限），但它只弹这一次。

安装时勾的「开机自动启动」写在当前用户的注册表里（不是全局），
所以任务管理器的「启动」页里能看到、也能停用，不需要管理员。

> 没签名的安装包，别人双击时 Windows 会先弹一句「Windows 已保护你的电脑」，
> 得点「更多信息 → 仍要运行」。这是 SmartScreen 对未知发布者的默认拦截，
> 去掉只能买代码签名证书。

想从命令行静默安装，**Git Bash 里要加 `MSYS_NO_PATHCONV=1`**：

```bash
MSYS_NO_PATHCONV=1 ./ClipNote-v1.0.0-setup.exe /VERYSILENT /TASKS="autostart,desktopicon"
```

不加的话 MSYS 会把 `/VERYSILENT` 这类以斜杠开头的参数**当成 Unix 路径改写掉**
（变成 `D:/Program Files/Git/VERYSILENT`），安装程序收不到任何开关，
结果是明明写了 `/VERYSILENT` 却弹出图形界面、`/TASKS` 也被忽略。
在 PowerShell 或 cmd 里没有这个问题。

## 发一个新版本

1. 改 [CMakeLists.txt](CMakeLists.txt) 第 7 行的版本号 —— **版本号全项目只有这一处**，
   `appVersion()`、「关于」对话框、安装包文件名都从它来，不用改别的地方
2. `powershell -File tools/build-installer.ps1` —— 编译、铺依赖、打安装包、压 zip 一次跑完
3. 在 GitHub 上发 Release，**tag 填 `v1.0.1`**
4. 把两个产物**都**传成附件

第 4 步为什么是「都传」：装了的用户和没装的用户要的东西不一样。更新器自己会挑 ——
它优先拿 `.zip`，名字里带 `setup` / `install` 的一律跳过（挑错了会把用户的主程序
替换成安装程序本身）。

要给内部测试的人先试，别在 tag 上挂 rc 后缀，勾 GitHub 那个
**Set as a pre-release** 就行 —— 更新器查的是 `/releases/latest`，它会自动跳过预发布。

**装到 `Program Files` 这类需要管理员权限的地方时自动更新会失败** —— 程序会在下载之前
就发现目录写不进去并告诉你，让你走「打开下载页」手动换。

> **面板故意不抢焦点**，所以你原来的程序一直保持着光标 —— 这也意味着
> **面板收不到键盘输入**：没有搜索框、不能用方向键，只能鼠标点选。
> 这是「不抢焦点」的必然代价，Windows 自带的 Win+V 也是同样的取舍。

## 环境要求

本机已装好的环境（更全的环境说明见 `D:\QT\test\README.md`）：

| 组件 | 版本 | 路径 |
|---|---|---|
| Qt | 6.8.3 (LTS) | `D:\Program Files\QT\6.8.3\mingw_64` |
| 编译器 MinGW | 13.1.0 | `D:\Program Files\QT\Tools\mingw1310_64` |
| CMake | 3.30.5 | `D:\Program Files\QT\Tools\CMake_64` |
| Ninja | 1.12.1 | `D:\Program Files\QT\Tools\Ninja` |

VS Code 插件：`ms-vscode.cpptools`、`ms-vscode.cmake-tools`、`theqtcompany.qt-cpp-pack`、`tonka3000.qtvsctools`

用到 Qt 的 `Core / Gui / Widgets / Network`。`Network` 是「检查更新」拉 GitHub 用的，
属于 Qt 的基础模块，装了 Qt 就有，不需要另外下载。HTTPS 走 Qt 自带的 Windows
Schannel 后端，不需要 OpenSSL。

## 构建和运行

这些工具都不在系统 PATH 上，先导出：

```bash
export PATH="/d/Program Files/QT/6.8.3/mingw_64/bin:/d/Program Files/QT/Tools/mingw1310_64/bin:/d/Program Files/QT/Tools/CMake_64/bin:/d/Program Files/QT/Tools/Ninja:$PATH"
```

```bash
cmake --preset mingw-debug            # 首次，或改过 CMakeLists.txt 之后
cmake --build --preset mingw-debug    # 产物：build/debug/ClipNote.exe
```

要 release 就换 `mingw-release`，产物在 `build/release/ClipNote.exe`。
编译器、Qt 路径、Ninja 生成器都写在 `CMakePresets.json` 里，不需要额外的 `-D` 参数。

**直接双击 exe 会因为找不到 `Qt6Widgets.dll` 而失败**，两种办法：

- 在 VS Code 里按 `F5` 调试（`launch.json` 已经把 Qt 的 bin 目录加进 PATH 了）
- 或者把依赖 DLL 拷到 exe 旁边：
  ```bash
  "/d/Program Files/QT/6.8.3/mingw_64/bin/windeployqt.exe" build/debug/ClipNote.exe
  ```

VS Code 里还可以：`Ctrl+Shift+P` → `CMake: Select Configure Preset` → 选
**Qt 6.8.3 MinGW - Debug**，然后 `F7` 构建、`F5` 调试。

## 自检：怎么确认环境是好的

程序是无控制台的 GUI 程序，`qDebug()` 的输出哪儿也去不了，所以带了个自检模式：

```bash
./build/debug/ClipNote.exe --selftest    # 退出码 0 = 全部通过
```

结果写在当前目录的 `selftest.log`。检查项：

- Qt 平台插件、系统托盘、剪贴板读写、默认全局热键能否注册
- **剪贴板历史的存储逻辑**（最新在前、去重、空白忽略、越界不崩、超上限截断、删除单条）——
  `ClipStore` 是纯数据、刻意不碰剪贴板，所以能直接构造出来灌数据验证
- **点条目写回剪贴板不会让列表重排**，而且这个抑制只对「我们自己写的那一次」生效：
  用户自己再复制同样的内容，照样会置顶
- **版本号解析与比较**（`v` 前缀、四段式 tag、预发布后缀、解析不出来的串必须被拒绝）
- **解析 GitHub 的 release 响应**——用的是真实抓下来的响应精简过的样本，
  所以它同时也在验字段名写得对不对
- **自更新解包那一步**：真造一个 zip 和一个裸 exe 进去，验能不能定位到主程序、
  以及不是可执行文件 / 包里没有主程序时会不会被挡下来
- 参考信息：几个候选热键在本机是否空闲

> **自动粘贴不在自检范围内**，而且是被刻意排除的：它要往当时拥有焦点的窗口里注入
> `Ctrl+V`，跑一次就可能把内容敲进你正在编辑的文档。日志里会留一行 `[ -- ]` 说明这一点。
> 想确认它工作，只能照「怎么用」那四步实际走一遍。

另有两个脚本，各管一块自检够不着的东西：

```bash
python tools/check-panel.py     # 面板的窗口性质（不注入任何输入）
python tools/test-updater.py    # 自更新的「换文件」那一步
```

`test-updater.py` 要单独说一句，因为它是这个仓库里**唯一会真的替换掉一个正在运行的程序**
的测试。它不碰 ClipNote 本身：编两个桩程序当替身，把**同一份**
[resources/apply-update.ps1](resources/apply-update.ps1) 对着它们跑三个场景 ——
正常更新、新版本起不来、新版本根本不是可执行文件。后两个验的是回滚能不能把旧版本救回来。
自检里跑不了这个（自检就跑在那个要被替换的程序里），所以拆出来单独验。

## 全局热键的两个坑

1. **热键是系统级独占的**，同一个组合被别的程序占了就注册不上。
   本机实测 `Ctrl+Alt+C` 已经被别的程序占用。默认用的是 `Alt+Q`。
   要换组合：先跑 `--selftest` 看哪个空闲，再改 [src/AppConfig.h](src/AppConfig.h)
   （主程序和自检共用这一处）。
   注册失败时程序不会退出，只是降级成「只能用托盘菜单」，并弹个气泡提示。

   > 用 `Alt+单个字母` 的副作用比组合键大：它会把别的程序里对应的**菜单助记键**
   > 一并抢走（比如 `Alt+Q` 在不少程序里是菜单项）。挑它就意味着接受这点。

2. **Qt 里 Windows 键写作 `Meta` 而不是 `Win`**。
   `QKeySequence("Win+Shift+C")` 会解析成空序列，注册必然失败。

## 自动粘贴的几条边界

下面这些是已知行为，不是 bug：

- **目标程序以管理员权限运行时粘不进去**。Windows 的 UIPI 会挡掉注入，ClipNote 不硬来，
  静默退化成「内容已在剪贴板，你自己按 `Ctrl+V`」。
- **`Alt` 还按着的时候不会硬粘**。虽然面板不抢焦点、Alt 基本早就松开了，但万一是
  用 `Alt+Q` 唤出后立刻点条目，合成的会变成 `Alt+Ctrl+V`。ClipNote 会先等 `Alt` 抬起
  （最多 600ms），等不到就干脆不粘。
- **`Alt+Q` 这个热键本身可能触发别的程序的菜单**。Alt 的按下和抬起都落在那边的窗口上
  （面板不抢焦点，也就吃不到这两个事件），这是 `Alt+单个字母` 类热键的通病。
  嫌烦就换个带 Ctrl 的组合，见上面「全局热键的两个坑」。

## 为什么标题栏是自己画的

因为**系统的标题栏加不了自定义按钮**。Windows 11 的标题栏由 DWM 合成，程序在
`WM_NCPAINT` 里往窗口上画的东西会被直接盖掉 —— 这是实测过的：写了个纯 Win32 探针窗口
（非 Qt），在标题栏上画色块，**屏幕上一个像素都没出现**。

想要非原生的标题栏外观，就只能把整条标题栏自己画，窗口随之变成无边框。
代价是**失去了系统提供的拖拽调整窗口大小**（面板做成了固定尺寸 360×440），
也没有原生的投影和圆角。换来的是外观完全可控。

## 目录结构

```
src/
  main.cpp                    程序入口：装配 store / 托盘 / 面板 / 全局热键
  AppConfig.h                 默认热键、自动粘贴开关等全局配置
  SelfTest.{h,cpp}            --selftest 自检
  model/
    ClipStore.{h,cpp}         剪贴板历史（QAbstractListModel，纯数据，不碰剪贴板）
    ClipboardWatcher.{h,cpp}  监听系统剪贴板，往 store 里喂
  platform/
    GlobalHotkey.{h,cpp}      系统级热键（Win32 RegisterHotKey + Qt 事件过滤器）
    AutoPaste.{h,cpp}         面板用到的 Win32 层：注入 Ctrl+V、轮询鼠标、
                              把窗口设成不激活。**全项目唯一注入输入的文件**
  update/
    Version.h                 版本号解析与比较（纯函数）
    ReleaseInfo.{h,cpp}       解析 GitHub release 响应 + 挑升级附件（纯函数）
    UpdateChecker.{h,cpp}     查 releases/latest、下载附件并校验
    UpdateInstaller.{h,cpp}   解包定位、写替换脚本、脱手启动它
  ui/
    MainWindow.{h,cpp}        剪贴板面板（不抢焦点，鼠标点选）
    TitleBar.{h,cpp}          自绘标题栏（图标 + 标题 + 齿轮菜单 + 关闭）
    ClipDelegate.{h,cpp}      剪贴板历史每一行的画法
    AboutDialog.{h,cpp}       「关于 ClipNote」
    UpdateDialog.{h,cpp}      检查更新的对话框，也是整条更新流程的控制器
    AppIcon.{h,cpp}           程序图标（读 resources/clipnote.png），托盘和标题栏共用
    Tray.{h,cpp}              托盘图标 + 右键菜单，程序的主入口
resources/
    resources.qrc             资源清单
    clipnote.png              程序图标
    apply-update.ps1          自更新收尾脚本的模板
tools/
    check-panel.py            面板窗口性质的进程外验证
    test-updater.py           自更新换文件那一步的验证
```

`AutoPaste` 是唯一会注入输入的文件，自检**不包含它的头**，从源头上就够不着注入路径。

> `MainWindow` 里显式调 `SetWindowLongPtr(GWL_EXSTYLE, ... | WS_EX_NOACTIVATE)` 是有原因的：
> `Qt::WindowDoesNotAcceptFocus` 在本机实测**没有真的落到窗口样式上**（查 exstyle 只有
> TOOLWINDOW/TOPMOST/WINDOWEDGE）。缺了它，用户一点面板就会把面板激活，紧接着注入的
> `Ctrl+V` 会打到面板自己身上 —— 表现就是「点了没反应」。别把这行当成冗余删掉。

程序图标是 [resources/clipnote.png](resources/clipnote.png)（那个绿色「复制」图标），
通过 [resources/resources.qrc](resources/resources.qrc) 编进程序，托盘和自绘标题栏共用。
**换图标就是替换这个 png 再重新构建**，不用动 CMakeLists。
注意它要缩到 16px 做托盘图标，用形状简单、色块大的图。

新增 `.cpp` 必须手工列进 `CMakeLists.txt` —— 源文件是显式列举的，没有用 GLOB。
