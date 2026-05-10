#include "mcplogdlg.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "../mcp/mcpserver.h"

namespace pv {
namespace dialogs {

McpLogDialog::McpLogDialog(QWidget *parent, mcp::McpServer *server)
    : DSDialog(parent, true, true)
    , _server(server)
{
    setTitle(tr("MCP Logs"));
    setMinimumSize(640, 360);

    QWidget *body = new QWidget(this);
    layout()->addWidget(body);
    QVBoxLayout *v = new QVBoxLayout(body);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(6);

    _status = new QLabel(body);
    _status->setObjectName("mcp_log_status");
    v->addWidget(_status);

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

    if (_server) {
        connect(_server, &mcp::McpServer::logMessage,
                this, &McpLogDialog::append_line);
    }

    refresh_status();
}

McpLogDialog::~McpLogDialog() = default;

void McpLogDialog::refresh_status()
{
    if (!_status) return;
    if (!_server) {
        _status->setText(tr("MCP server: not initialised"));
        return;
    }
    if (_server->isListening()) {
        _status->setText(tr("MCP server: listening on 127.0.0.1:%1")
                         .arg(_server->listenPort()));
    } else {
        _status->setText(tr("MCP server: stopped"));
    }
}

void McpLogDialog::append_line(const QString &line)
{
    if (_text) _text->appendPlainText(line);
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

} // namespace dialogs
} // namespace pv
