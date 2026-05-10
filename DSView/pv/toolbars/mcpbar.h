/*
 * Standalone toolbar entry for the embedded MCP server. Sits between
 * FileBar and LogoBar in the main window so the user can toggle the
 * server and open the log dialog without going through the Help menu.
 */
#ifndef DSVIEW_PV_TOOLBARS_MCPBAR_H
#define DSVIEW_PV_TOOLBARS_MCPBAR_H

#include <QAction>
#include <QMenu>
#include <QToolBar>

#include "../sigsession.h"
#include "../ui/uimanager.h"
#include "../ui/xtoolbutton.h"

namespace pv {
namespace toolbars {

class McpBar : public QToolBar, public IUiWindow
{
    Q_OBJECT

public:
    explicit McpBar(SigSession *session, QWidget *parent = nullptr);
    ~McpBar() override;

    /* Reflect the live server state in the UI (called by MainWindow
     * after it knows whether McpServer::start succeeded). */
    void set_mcp_listening(bool on);

signals:
    void sig_mcp_toggle(bool on);
    void sig_mcp_show_log();

private slots:
    void on_toggle(bool checked);
    void on_show_log();

private:
    void retranslateUi();
    void reStyle();

    void UpdateLanguage() override;
    void UpdateTheme() override;
    void UpdateFont() override;

    SigSession *_session;
    XToolButton _mcp_button;
    QMenu      *_menu;
    QAction    *_action_toggle;   // checkable
    QAction    *_action_log;
};

} // namespace toolbars
} // namespace pv

#endif // DSVIEW_PV_TOOLBARS_MCPBAR_H
