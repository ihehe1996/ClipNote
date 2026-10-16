cmake_minimum_required(VERSION 3.21)

# 版本号**只有这一处**。发新版就改这一行，然后打一个同样名字的 tag（比如 v1.0.1）。
#
# 注意这里必须写纯数字：CMake 按 major.minor.patch 解析，写 "v1.0.0" 会报错。
# 对外的 "v" 前缀在下面用编译期宏补上，见 CLIPNOTE_VERSION。
project(ClipNote VERSION 1.0.1 LANGUAGES C CXX)

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
