/*
 * Modeless log viewer for the embedded MCP server.
 * Subscribes to McpServer::logMessage and appends each line to a
 * scrolling QPlainTextEdit. The dialog can be safely shown / hidden
 * many times — it does not own the McpServer.
 */
#ifndef DSVIEW_PV_DIALOGS_MCPLOGDLG_H
#define DSVIEW_PV_DIALOGS_MCPLOGDLG_H

#include <QPointer>
#include <QString>

#include "dsdialog.h"

class QPlainTextEdit;
class QPushButton;
class QLabel;

namespace pv {
namespace mcp { class McpServer; }

namespace dialogs {

class McpLogDialog : public DSDialog
{
    Q_OBJECT

public:
    McpLogDialog(QWidget *parent, mcp::McpServer *server);
    ~McpLogDialog() override;

    void refresh_status();

public slots:
    void append_line(const QString &line);

private slots:
    void on_clear_clicked();
    void on_copy_clicked();

private:
    QPointer<mcp::McpServer> _server;
    QPlainTextEdit          *_text       = nullptr;
    QLabel                  *_status     = nullptr;
    QPushButton             *_btn_clear  = nullptr;
    QPushButton             *_btn_copy   = nullptr;
};

} // namespace dialogs
} // namespace pv

#endif // DSVIEW_PV_DIALOGS_MCPLOGDLG_H
