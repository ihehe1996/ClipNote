#include "ui/AboutDialog.h"

#include "AppConfig.h"
#include "ui/AppIcon.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSysInfo>
#include <QVBoxLayout>

namespace ClipNote {
namespace {

// 编译器名，只为了写在「关于」里，出问题时能一眼看出是哪个构建
QString compilerName()
{
#if defined(__MINGW32__) || defined(__MINGW64__)
    return QStringLiteral("MinGW GCC %1.%2.%3")
        .arg(__GNUC__)
        .arg(__GNUC_MINOR__)
        .arg(__GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    return QStringLiteral("MSVC %1").arg(_MSC_VER);
#else
    return QStringLiteral("未知编译器");
#endif
}

QString orDash(const QString &text)
{
    return text.isEmpty() ? QStringLiteral("—") : text;
}

} // namespace

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

    auto *copyInfo = new QPushButton(QStringLiteral("复制版本信息"), this);
    copyInfo->setToolTip(QStringLiteral("把这台机器上的版本、Qt、编译器信息复制到剪贴板，报问题时贴这个"));
    connect(copyInfo, &QPushButton::clicked, this, [this, copyInfo] {
        QApplication::clipboard()->setText(buildVersionInfo());
        copyInfo->setText(QStringLiteral("已复制"));
        copyInfo->setEnabled(false);
    });

    auto *buttons = new QDialogButtonBox(this);
    buttons->addButton(copyInfo, QDialogButtonBox::ActionRole);
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

QString AboutDialog::buildVersionInfo()
{
    return QStringLiteral(
               "ClipNote %1\n"
               "Qt %2\n"
               "%3\n"
               "%4\n"
               "热键 %5")
        .arg(QCoreApplication::applicationVersion(),
             QString::fromLatin1(qVersion()),
             compilerName(),
             QSysInfo::prettyProductName(),
             orDash(defaultHotkey()));
}

} // namespace ClipNote
