#include "ui/AboutDialog.h"

#include "AppConfig.h"
#include "ui/AppIcon.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace ClipNote {

AboutDialog::AboutDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("关于 ClipNote"));
    // 面板是永远置顶的，对话框不跟着置顶就会被它压住
    setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint);
    setMinimumWidth(400);

    auto *icon = new QLabel(this);
    icon->setPixmap(appIcon().pixmap(64, 64));
    icon->setFixedSize(64, 64);

    auto *name = new QLabel(QStringLiteral("ClipNote"), this);
    name->setStyleSheet(QStringLiteral("font-size: 20px; font-weight: 600; color: #1f1f1f;"));

    auto *version = new QLabel(
        QStringLiteral("版本 %1").arg(QCoreApplication::applicationVersion()), this);
    version->setStyleSheet(QStringLiteral("color: #6b6b6b;"));

    auto *tagline = new QLabel(
        QStringLiteral("一个常驻托盘的剪贴板历史工具。"), this);
    tagline->setStyleSheet(QStringLiteral("color: #3a3a3a;"));

    auto *headerText = new QVBoxLayout;
    headerText->setSpacing(2);
    headerText->addWidget(name);
    headerText->addWidget(version);
    headerText->addSpacing(6);
    headerText->addWidget(tagline);
    headerText->addStretch(1);

    auto *header = new QHBoxLayout;
    header->setSpacing(16);
    header->addWidget(icon);
    header->addLayout(headerText, 1);

    auto *details = new QFormLayout;
    details->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    details->setHorizontalSpacing(12);
    details->setVerticalSpacing(6);
    details->addRow(QStringLiteral("开发语言"), new QLabel(QStringLiteral("C++"), this));
    details->addRow(QStringLiteral("默认热键"),
                    new QLabel(defaultHotkey().toHtmlEscaped(), this));
    {
        auto *homeLabel = new QLabel(
            QStringLiteral("<a href=\"https://www.boshou.me\">www.boshou.me</a>"), this);
        homeLabel->setOpenExternalLinks(true);
        homeLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
        details->addRow(QStringLiteral("作者主页"), homeLabel);
    }
    // QQ 就是一行文字，不做成链接：点它得靠系统注册了 tencent:// 协议，
    // 没装 QQ 或者没注册的机器上点一下毫无反应，比不能点更让人困惑。
    details->addRow(QStringLiteral("作者 QQ"), new QLabel(QStringLiteral("10220739"), this));

    auto *buttons = new QDialogButtonBox(this);
    QPushButton *close = buttons->addButton(QStringLiteral("关闭"), QDialogButtonBox::RejectRole);
    close->setDefault(true);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 16);
    layout->setSpacing(16);
    layout->addLayout(header);
    layout->addLayout(details);
    layout->addStretch(1);
    layout->addWidget(buttons);
}

} // namespace ClipNote
