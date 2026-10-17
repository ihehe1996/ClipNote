cmake_minimum_required(VERSION 3.21)

# 版本号**只有这一处**。发新版就改这一行，然后打一个同样名字的 tag（比如 v1.0.1）。
#
# 注意这里必须写纯数字：CMake 按 major.minor.patch 解析，写 "v1.0.0" 会报错。
# 对外的 "v" 前缀在下面用编译期宏补上，见 CLIPNOTE_VERSION。
project(ClipNote VERSION 1.0.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 让 CMake 自动处理 Qt 的元对象编译器(moc)、资源(rcc)和界面(uic)。
# 新建带 Q_OBJECT 的头文件、.qrc 或 .ui 时，只要加进下面的源文件列表即可。
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Network 是「检查更新」用的：拉 GitHub 的 release JSON、下载附件。
# 它是 Qt 的基础模块（不是附加模块），装了 Qt 就有，不用另外下载。
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)

qt_standard_project_setup()

# 程序图标：resources/clipnote.rc 编译进 PE 的资源段。桌面/开始菜单快捷方式、
# 任务栏和「应用和功能」里的图标认的就是那一份 —— 而 resources.qrc 里那个 png
# 只管运行期的 QIcon（托盘、标题栏），两边不能互替。
# windres 就在编译器旁边，CMake 自己找得到，presets 里不用加东西。
#
# **为什么单独编成一个小目标，而不是塞进下面 qt_add_executable 的源列表**：
# 塞进去它会继承 ClipNote 的 include 目录，而 **windres 把这些目录原样转给预处理器
# 时不加引号** —— Qt 的头目录正是 "D:/Program Files/QT/..."，带空格，于是命令行在
# 空格处被截断，预处理必然失败。它报的是
#     cc1.exe: fatal error: Files/QT/6.8.3/mingw_64/include: No such file or directory
# 看着像 Qt 头文件找不到，其实是 windres 不加引号。实测过：同一份 .rc 把带空格的
# -I 摘掉就正常编出目标文件，加回去就报这条。
# 这个对象库不链接 Qt，编 .rc 时本来就没有 Qt 那些带空格的 -I。
# 但那还不够：autogen 会给目标塞一个 `-I <构建目录>/clipnote_icon_autogen/include`，
# 而构建目录同样可能带空格（比如仓库放在 `C:\My Projects\` 下）。所以这里把
# AUTOMOC/AUTOUIC/AUTORCC 全关掉 —— 这个目标只有 .rc，三个都用不上。关掉之后
# 那条 RC 编译规则里一条 -I 都没有（已在 build.ninja 里核对过），也就不会再踩。
# （顺带：.rc 里那个 ICON "clipnote.ico" 的相对路径是相对 .rc 自己的目录解析的，
#  不是相对构建目录 —— 所以 ico 放在 .rc 旁边就够了，不用额外给 -I。）
#
# 另一个不能塞进上面那个源列表的原因：那段列表没有平台条件，而 RC 语言只有 Windows 有。
if(WIN32)
    enable_language(RC)
    add_library(clipnote_icon OBJECT resources/clipnote.rc)
    set_target_properties(clipnote_icon PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
endif()

# WIN32 表示生成不带控制台窗口的 GUI 程序。
# 代价是 qDebug() 的输出看不见，所以程序另有一个 --selftest 自检模式，
# 结果写进当前目录的 selftest.log。
qt_add_executable(ClipNote WIN32
    src/main.cpp

    src/SelfTest.h
    src/SelfTest.cpp

    src/model/ClipStore.h
    src/model/ClipStore.cpp

    src/model/ClipHistoryFile.h
    src/model/ClipHistoryFile.cpp

    src/model/ClipFilterProxy.h
    src/model/ClipFilterProxy.cpp

    src/model/ClipImageFile.h
    src/model/ClipImageFile.cpp

    src/model/ClipPageProxy.h
    src/model/ClipPageProxy.cpp

    src/model/ClipboardWatcher.h
    src/model/ClipboardWatcher.cpp

    src/platform/GlobalHotkey.h
    src/platform/GlobalHotkey.cpp

    src/platform/AutoPaste.h
    src/platform/AutoPaste.cpp

    src/platform/WindowResizer.h
    src/platform/WindowResizer.cpp

    src/update/Version.h

    src/update/ReleaseInfo.h
    src/update/ReleaseInfo.cpp

    src/update/UpdateChecker.h
    src/update/UpdateChecker.cpp

    src/update/UpdateInstaller.h
    src/update/UpdateInstaller.cpp

    src/ui/AppIcon.h
    src/ui/AppIcon.cpp

    src/ui/TitleBar.h
    src/ui/TitleBar.cpp

    src/ui/ClipDelegate.h
    src/ui/ClipDelegate.cpp

    src/ui/PreviewPopup.h
    src/ui/PreviewPopup.cpp

    src/ui/AboutDialog.h
    src/ui/AboutDialog.cpp

    src/ui/UpdateDialog.h
    src/ui/UpdateDialog.cpp

    src/ui/MainWindow.h
    src/ui/MainWindow.cpp

    src/ui/Tray.h
    src/ui/Tray.cpp

    resources/resources.qrc
)

# 把图标那个对象库的目标文件并进 exe（见上面 clipnote_icon 那段的说明）
if(WIN32)
    target_link_libraries(ClipNote PRIVATE clipnote_icon)
endif()

target_include_directories(ClipNote PRIVATE src)

# 把版本号以编译期宏的形式交给 C++ 那边（见 src/AppConfig.h 的 appVersion()）。
#
# 为什么要绕这一下：CMake 的 project(VERSION) 只吃纯数字，而对外的写法带 v 前缀，
# 两个地方各写一遍必然会漂 —— 那种漂的后果不是显示难看，是**更新器比错版本**：
# exe 里写的是旧号，用户更新完还被告知有新版本，陷入死循环。
target_compile_definitions(ClipNote PRIVATE
    CLIPNOTE_VERSION="v${PROJECT_VERSION}"
)

# 打包脚本（tools/build-installer.ps1）要拿版本号给安装包命名，从这儿读。
# 让 CMake 把版本号落成一个文件，而不是让脚本去解析 CMakeLists —— 解析文本很脆。
file(WRITE "${CMAKE_BINARY_DIR}/version.txt" "${PROJECT_VERSION}\n")

target_link_libraries(ClipNote PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
)

# user32：全局热键（RegisterHotKey/WM_HOTKEY）、窗口样式、SendInput
# dwmapi：请 DWM 把无边框窗口画成圆角
if(WIN32)
    target_link_libraries(ClipNote PRIVATE user32 dwmapi)
endif()

# 把 Qt 的 DLL 拷到 exe 旁边，方便直接双击运行
if(WIN32)
    qt_generate_deploy_app_script(
        TARGET ClipNote
        OUTPUT_SCRIPT deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
    )
    install(SCRIPT ${deploy_script})
endif()
