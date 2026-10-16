#include "ui/Tray.h"

#include "ui/AppIcon.h"

#include <QAction>
#include <QIcon>
#include <QMenu>
#include <QSystemTrayIcon>

namespace ClipNote {

Tray::Tray(QObject *parent) : QObject(parent)
{
    // 图标和自绘标题栏左上角共用同一个（见 AppIcon.h）
    m_icon = new QSystemTrayIcon(appIcon(), this);
    m_icon->setToolTip(QStringLiteral("ClipNote"));

    // QMenu 是 QWidget，父对象只能是 QWidget，所以这里不能挂到 this 上
    m_menu = new QMenu;

    QAction *toggle = m_menu->addAction(QStringLiteral("显示 / 隐藏面板"));
    connect(toggle, &QAction::triggered, this, &Tray::toggleRequested);

    m_hotkeyInfo = m_menu->addAction(QStringLiteral("热键：—"));
    m_hotkeyInfo->setEnabled(false);

    m_menu->addSeparator();

    // 没配更新源（AppConfig.h 里的 updateRepo）就先别摆出来 ——
    // 一个点了只会说「还没配好」的菜单项不如没有
    m_updateAction = m_menu->addAction(QStringLiteral("检查更新…"));
    m_updateAction->setVisible(false);
    connect(m_updateAction, &QAction::triggered, this, &Tray::checkUpdateRequested);

    m_menu->addSeparator();

    QAction *quit = m_menu->addAction(QStringLiteral("退出 ClipNote"));
    connect(quit, &QAction::triggered, this, &Tray::quitRequested);

    m_icon->setContextMenu(m_menu);

    // 左键单击托盘图标也切换面板，Windows 上的习惯用法
    connect(m_icon, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger)
                    emit toggleRequested();
            });
}

Tray::~Tray()
{
    delete m_menu;
    m_menu = nullptr;
}

bool Tray::isAvailable()
{
    return QSystemTrayIcon::isSystemTrayAvailable();
}

void Tray::show()
{
    m_icon->show();
}

void Tray::showMessage(const QString &title, const QString &body)
{
    m_icon->showMessage(title, body, QSystemTrayIcon::Information, 5000);
}

void Tray::setHotkeyText(const QString &text)
{
    m_hotkeyInfo->setText(QStringLiteral("热键：%1").arg(text));
}

void Tray::setUpdateActionVisible(bool visible)
{
    m_updateAction->setVisible(visible);
}

} // namespace ClipNote
