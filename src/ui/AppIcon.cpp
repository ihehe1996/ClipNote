#include "ui/AppIcon.h"

#include <QString>

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

QIcon appIcon()
{
    return QIcon(QStringLiteral(":/clipnote.png"));
}

bool hasEmbeddedAppIcon()
{
#ifdef Q_OS_WIN
    // 资源 ID 见 resources/clipnote.rc（那里写的是 #define IDI_CLIPNOTE 1）。
    // 取一次再销毁就够 —— 这里要回答的只是「资源段里有没有这么一份图标」。
    const HICON icon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1),
        IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    if (!icon)
        return false;
    DestroyIcon(icon);
    return true;
#else
    // 别的平台上没有 PE 资源段这回事，这条检查不适用
    return true;
#endif
}

} // namespace ClipNote
