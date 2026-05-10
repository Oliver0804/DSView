#include "mcplogdlg.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>

#include "../mcp/mcpserver.h"

namespace pv {
namespace dialogs {

namespace {
/* Coloured status pill — green when listening, red when stopped. Drawn
 * with a stylesheet on a tiny QLabel so we don't need a custom paint. */
constexpr const char *STATUS_DOT_ON =
    "QLabel { background-color:#00b15a; border-radius:7px; min-width:14px; "
    "max-width:14px; min-height:14px; max-height:14px; }";
constexpr const char *STATUS_DOT_OFF =
    "QLabel { background-color:#888888; border-radius:7px; min-width:14px; "
    "max-width:14px; min-height:14px; max-height:14px; }";
}

McpLogDialog::McpLogDialog(QWidget *parent, mcp::McpServer *server)
    : DSDialog(parent, true, true)
    , _server(server)
{
    setTitle(tr("MCP Logs"));
    setMinimumSize(700, 400);

    QWidget *body = new QWidget(this);
    layout()->addWidget(body);
    QVBoxLayout *v = new QVBoxLayout(body);
    v->setContentsMargins(10, 10, 10, 10);
    v->setSpacing(8);

    /* status row: ●  text                        [Start/Stop] */
    QHBoxLayout *status_row = new QHBoxLayout();
    status_row->setSpacing(8);
    _status_dot = new QLabel(body);
    _status_dot->setStyleSheet(STATUS_DOT_OFF);
    status_row->addWidget(_status_dot);
    _status = new QLabel(body);
    _status->setObjectName("mcp_log_status");
    status_row->addWidget(_status, 1);
    _btn_toggle = new QPushButton(tr("Start"), body);
    _btn_toggle->setMinimumWidth(80);
    status_row->addWidget(_btn_toggle);
    v->addLayout(status_row);

    _text = new QPlainTextEdit(body);
    _text->setReadOnly(true);
    _text->setMaximumBlockCount(5000);
    _text->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont f = _text->font();
    f.setStyleHint(QFont::Monospace);
    f.setFamily("Menlo");
    _text->setFont(f);
    v->addWidget(_text, 1);

    QHBoxLayout *btn_row = new QHBoxLayout();
    btn_row->addStretch(1);
    _btn_copy = new QPushButton(tr("Copy"), body);
    _btn_clear = new QPushButton(tr("Clear"), body);
    btn_row->addWidget(_btn_copy);
    btn_row->addWidget(_btn_clear);
    v->addLayout(btn_row);

    connect(_btn_clear, &QPushButton::clicked,
            this, &McpLogDialog::on_clear_clicked);
    connect(_btn_copy, &QPushButton::clicked,
            this, &McpLogDialog::on_copy_clicked);
    connect(_btn_toggle, &QPushButton::clicked,
            this, &McpLogDialog::on_toggle_clicked);

    if (_server) {
        connect(_server, &mcp::McpServer::logMessage,
                this, &McpLogDialog::append_line);
        connect(_server, &mcp::McpServer::listeningChanged,
                this, &McpLogDialog::on_listening_changed);
    }

    refresh_status();
}

McpLogDialog::~McpLogDialog() = default;

bool McpLogDialog::is_scrolled_to_bottom() const
{
    if (!_text) return true;
    QScrollBar *sb = _text->verticalScrollBar();
    if (!sb) return true;
    /* Treat "within a few lines of the bottom" as still tracking — the
     * user may have lagged the scroll a touch but still want to follow. */
    return (sb->value() >= sb->maximum() - 16);
}

void McpLogDialog::scroll_to_bottom()
{
    if (!_text) return;
    QScrollBar *sb = _text->verticalScrollBar();
    if (sb) sb->setValue(sb->maximum());
}

void McpLogDialog::refresh_status()
{
    if (!_status || !_status_dot || !_btn_toggle) return;
    if (!_server) {
        _status->setText(tr("MCP server: not initialised"));
        _status_dot->setStyleSheet(STATUS_DOT_OFF);
        _btn_toggle->setText(tr("Start"));
        _btn_toggle->setEnabled(false);
        return;
    }
    if (_server->isListening()) {
        _status->setText(tr("Listening on 127.0.0.1:%1")
                         .arg(_server->listenPort()));
        _status_dot->setStyleSheet(STATUS_DOT_ON);
        _btn_toggle->setText(tr("Stop"));
    } else {
        _status->setText(tr("Server stopped"));
        _status_dot->setStyleSheet(STATUS_DOT_OFF);
        _btn_toggle->setText(tr("Start"));
    }
    _btn_toggle->setEnabled(true);
}

void McpLogDialog::append_line(const QString &line)
{
    if (!_text) return;
    /* Auto-scroll only when the user is already at the bottom — never
     * yank the viewport away while they're reading earlier output. */
    bool stick = is_scrolled_to_bottom();
    _text->appendPlainText(line);
    if (stick) scroll_to_bottom();
}

void McpLogDialog::on_listening_changed(bool listening)
{
    Q_UNUSED(listening);
    refresh_status();
}

void McpLogDialog::on_clear_clicked()
{
    if (_text) _text->clear();
}

void McpLogDialog::on_copy_clicked()
{
    if (!_text) return;
    QApplication::clipboard()->setText(_text->toPlainText());
}

void McpLogDialog::on_toggle_clicked()
{
    if (!_server) return;
    if (_server->isListening()) {
        _server->stop();
    } else {
        /* Re-use whatever port the server is configured for; if it's
         * never been started, default to 7384 (matches MainWindow). */
        quint16 port = _server->listenPort() ? _server->listenPort() : 7384;
        if (!_server->start(port)) {
            /* listeningChanged is not emitted on failure; refresh
             * manually so the button label resets. */
            refresh_status();
        }
    }
}

} // namespace dialogs
} // namespace pv
