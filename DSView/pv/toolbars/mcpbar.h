/*
 * Standalone toolbar entry for the embedded MCP server. Sits between
 * FileBar and LogoBar in the main window so the user can toggle the
 * server and open the log dialog without going through the Help menu.
 *
 * Two directly-clickable actions (no nested menu):
 *   1. MCP — checkable toggle. Icon swaps between mcp_on.svg /
 *      mcp_off.svg so the listening state is visible at a glance.
 *   2. Logs — opens the log dialog.
 */
#ifndef DSVIEW_PV_TOOLBARS_MCPBAR_H
#define DSVIEW_PV_TOOLBARS_MCPBAR_H

#include <QAction>
#include <QPointer>
#include <QToolBar>

#include "../sigsession.h"
#include "../ui/uimanager.h"

namespace pv {
namespace mcp { class McpServer; }

namespace toolbars {

class McpBar : public QToolBar, public IUiWindow
{
    Q_OBJECT

public:
    explicit McpBar(SigSession *session, QWidget *parent = nullptr);
    ~McpBar() override;

    /* Reflect the live server state in the UI (icon + tooltip + check
     * state). Safe to call from any thread that owns the GUI thread —
     * intended to be called from MainWindow after start/stop or via
     * the McpServer::listeningChanged signal. */
    void set_mcp_listening(bool on);

    /* Wire the bar directly to the server so it auto-updates whenever
     * the server's listening state flips. Optional — set_mcp_listening
     * still works when called from MainWindow. */
    void attach_server(pv::mcp::McpServer *server);

signals:
    void sig_mcp_toggle(bool on);
    void sig_mcp_show_log();

private slots:
    void on_toggle(bool checked);
    void on_show_log();

private:
    void retranslateUi();
    void reStyle();
    void refresh_tooltip();

    void UpdateLanguage() override;
    void UpdateTheme() override;
    void UpdateFont() override;

    SigSession                *_session;
    QPointer<pv::mcp::McpServer> _server;

    QAction *_action_toggle = nullptr; // checkable
    QAction *_action_log    = nullptr;

    bool     _is_listening  = false;
    quint16  _last_port     = 7384;
};

} // namespace toolbars
} // namespace pv

#endif // DSVIEW_PV_TOOLBARS_MCPBAR_H
