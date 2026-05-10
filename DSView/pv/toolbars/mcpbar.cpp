#include "mcpbar.h"

#include <QFile>
#include <QIcon>
#include <QPushButton>

#include "../config/appconfig.h"
#include "../ui/fn.h"
#include "../ui/langresource.h"

namespace pv {
namespace toolbars {

McpBar::McpBar(SigSession *session, QWidget *parent)
    : QToolBar("MCP Bar", parent)
    , _session(session)
    , _mcp_button(this)
    , _menu(nullptr)
    , _action_toggle(nullptr)
    , _action_log(nullptr)
{
    setMovable(false);
    setContentsMargins(0, 0, 0, 0);

    _action_toggle = new QAction(this);
    _action_toggle->setObjectName(QString::fromUtf8("actionMcpToggle"));
    _action_toggle->setCheckable(true);
    _action_toggle->setChecked(false);

    _action_log = new QAction(this);
    _action_log->setObjectName(QString::fromUtf8("actionMcpLog"));

    _menu = new QMenu(this);
    _menu->setObjectName(QString::fromUtf8("menuMcp"));
    _menu->addAction(_action_toggle);
    _menu->addAction(_action_log);
    _mcp_button.setMenu(_menu);

    _mcp_button.setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    _mcp_button.setPopupMode(QToolButton::InstantPopup);

    addWidget(&_mcp_button);

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

void McpBar::retranslateUi()
{
    /* Use literal text — no language-resource entries shipped for the
     * MCP toolbar yet, so the existing L_S table would warn for every
     * id. Plain QString is fine because this label rarely needs to
     * change with locale (it's a brand-style entry like "MCP"). */
    _mcp_button.setText(tr("MCP"));
    _action_toggle->setText(tr("MCP Server (port 7384)"));
    _action_log->setText(tr("MCP Logs..."));
}

void McpBar::reStyle()
{
    QString iconPath = GetIconPath();
    /* Reuse the bug.svg icon as a placeholder — DSView's res/ doesn't
     * ship a dedicated MCP graphic. Falls back to no icon if missing. */
    QIcon icon(iconPath + "/bug.svg");
    if (!icon.isNull())
        _mcp_button.setIcon(icon);
}

void McpBar::set_mcp_listening(bool on)
{
    if (!_action_toggle) return;
    QSignalBlocker block(_action_toggle);
    _action_toggle->setChecked(on);
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
