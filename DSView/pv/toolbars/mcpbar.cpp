#include "mcpbar.h"

#include <QFile>
#include <QIcon>

#include "../config/appconfig.h"
#include "../mcp/mcpserver.h"
#include "../ui/fn.h"
#include "../ui/langresource.h"

namespace pv {
namespace toolbars {

McpBar::McpBar(SigSession *session, QWidget *parent)
    : QToolBar("MCP Bar", parent)
    , _session(session)
{
    setMovable(false);
    setContentsMargins(0, 0, 0, 0);

    /* Match the rest of the DSView toolbars (FileBar, LogoBar): icon
     * on top, text label below — otherwise the bare icons would look
     * out-of-place next to "File", "Help" etc. */
    setToolButtonStyle(Qt::ToolButtonTextUnderIcon);

    _action_toggle = new QAction(this);
    _action_toggle->setObjectName(QString::fromUtf8("actionMcpToggle"));
    _action_toggle->setCheckable(true);
    _action_toggle->setChecked(false);

    _action_log = new QAction(this);
    _action_log->setObjectName(QString::fromUtf8("actionMcpLog"));

    /* QToolBar::addAction renders each QAction as a QToolButton in the
     * bar — exactly the same look as SamplingBar's individual buttons,
     * so the MCP entries blend in instead of looking like a stray
     * dropdown. */
    addAction(_action_toggle);
    addAction(_action_log);

    connect(_action_toggle, SIGNAL(toggled(bool)),
            this, SLOT(on_toggle(bool)));
    connect(_action_log, SIGNAL(triggered()),
            this, SLOT(on_show_log()));

    ADD_UI(this);
}

McpBar::~McpBar()
{
    REMOVE_UI(this);
}

void McpBar::attach_server(pv::mcp::McpServer *server)
{
    if (_server == server) return;
    _server = server;
    if (_server) {
        connect(_server, &pv::mcp::McpServer::listeningChanged,
                this,    &McpBar::set_mcp_listening,
                Qt::UniqueConnection);
        set_mcp_listening(_server->isListening());
    }
}

void McpBar::retranslateUi()
{
    /* Plain tr() — the L_S table doesn't have entries for the MCP bar
     * yet; using L_S would just print "missing key" warnings. */
    _action_toggle->setText(tr("MCP"));
    _action_log->setText(tr("Logs"));
    refresh_tooltip();
}

void McpBar::reStyle()
{
    QString iconPath = GetIconPath();
    QString state = _is_listening ? "/mcp_on.svg" : "/mcp_off.svg";

    QString full = iconPath + state;
    if (QFile::exists(full)) {
        _action_toggle->setIcon(QIcon(full));
    }

    QString logIcon = iconPath + "/log.svg";
    if (QFile::exists(logIcon)) {
        _action_log->setIcon(QIcon(logIcon));
    }
}

void McpBar::refresh_tooltip()
{
    if (_action_toggle) {
        if (_is_listening) {
            _action_toggle->setToolTip(
                tr("MCP server: listening on 127.0.0.1:%1\n"
                   "Click to stop").arg(_last_port));
        } else {
            _action_toggle->setToolTip(
                tr("MCP server: stopped\n"
                   "Click to start (port %1)").arg(_last_port));
        }
    }
    if (_action_log) {
        _action_log->setToolTip(tr("Open MCP log viewer"));
    }
}

void McpBar::set_mcp_listening(bool on)
{
    if (_is_listening == on && _action_toggle && _action_toggle->isChecked() == on)
        return;

    _is_listening = on;
    if (_server)
        _last_port = _server->listenPort() ? _server->listenPort() : _last_port;

    if (_action_toggle) {
        QSignalBlocker block(_action_toggle);
        _action_toggle->setChecked(on);
    }
    reStyle();
    refresh_tooltip();
}

void McpBar::on_toggle(bool checked)
{
    emit sig_mcp_toggle(checked);
}

void McpBar::on_show_log()
{
    emit sig_mcp_show_log();
}

void McpBar::UpdateLanguage() { retranslateUi(); }
void McpBar::UpdateTheme()    { reStyle(); }
void McpBar::UpdateFont()
{
    QFont f = font();
    f.setPointSizeF(AppConfig::Instance().appOptions.fontSize);
    ui::set_toolbar_font(this, f);
}

} // namespace toolbars
} // namespace pv
