#include "ui/AppIcon.h"

#include <QString>

namespace ClipNote {

QIcon appIcon()
{
    return QIcon(QStringLiteral(":/clipnote.png"));
}

} // namespace ClipNote
