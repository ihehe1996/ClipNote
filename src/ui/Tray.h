#pragma once

#include <QObject>
#include <QString>

QT_BEGIN_NAMESPACE
class QAction;
class QMenu;
class QSystemTrayIcon;
QT_END_NAMESPACE

namespace ClipNote {

// 托盘图标和右键菜单。
//
// 这是程序的主入口：没有可见窗口时，用户能碰到的只有它。
// 图标是运行时用 QPainter 画的，不依赖外部资源文件 —— 换成美术给的图标时，
// 把 .qrc 加进 CMakeLists.txt 的源文件列表，资源系统会自动编译进去。
class Tray : public QObject
{
    Q_OBJECT

public:
    explicit Tray(QObject *parent = nullptr);
    ~Tray() override;

    static bool isAvailable();

    void show();
    void showMessage(const QString &title, const QString &body);

    // 菜单里那行「检查更新」，由 main.cpp 接上，好让气泡点一下就能接着走
    void setUpdateActionVisible(bool visible);

public slots:
    // 更新菜单里显示的热键文案；注册失败时显示失败原因
    void setHotkeyText(const QString &text);

signals:
    void toggleRequested();
    void quitRequested();
    void checkUpdateRequested();
    // 用户点了气泡消息本体（不是右键菜单）。用来把「发现新版本」的提示变成入口。
    void messageClicked();

private:
    QSystemTrayIcon *m_icon = nullptr;
    QMenu *m_menu = nullptr;        // 无父对象，析构里手动 delete
    QAction *m_hotkeyInfo = nullptr;
    QAction *m_updateAction = nullptr;
};

} // namespace ClipNote
