#include "mcpserver.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>

#include "../sigsession.h"
#include "../mainwindow.h"
#include "../eventobject.h"
#include "../log.h"

namespace pv {
namespace mcp {

McpServer::McpServer(SigSession *session, MainWindow *main_window,
                     QObject *parent)
    : QObject(parent)
    , _session(session)
    , _main_window(main_window)
{
    connect(&_server, &QTcpServer::newConnection,
            this,    &McpServer::onNewConnection);

    /* Hook into capture-end signal for async capture responses.
     * MainWindow exposes its EventObject via getEvent(). */
    if (_main_window) {
        EventObject *ev = _main_window->getEvent();
        if (ev) {
            connect(ev, SIGNAL(frame_ended()),
                    this, SLOT(onFrameEnded()),
                    Qt::QueuedConnection);
        }
    }
}

McpServer::~McpServer() = default;

bool McpServer::start(quint16 port)
{
    if (_server.isListening()) return true;

    if (!_server.listen(QHostAddress::LocalHost, port)) {
        dsv_err("MCP: failed to listen on 127.0.0.1:%u — %s",
                port, _server.errorString().toUtf8().data());
        return false;
    }
    dsv_info("MCP: listening on 127.0.0.1:%u", _server.serverPort());
    return true;
}

void McpServer::stop()
{
    _server.close();
}

void McpServer::onNewConnection()
{
    while (_server.hasPendingConnections()) {
        QTcpSocket *sock = _server.nextPendingConnection();
        sock->setProperty("_mcp_buf", QByteArray());
        connect(sock, &QTcpSocket::readyRead,
                this, &McpServer::onClientReadyRead);
        connect(sock, &QTcpSocket::disconnected,
                this, &McpServer::onClientDisconnected);
        dsv_info("MCP: client connected from %s",
                 sock->peerAddress().toString().toUtf8().data());
    }
}

void McpServer::onClientDisconnected()
{
    QTcpSocket *sock = qobject_cast<QTcpSocket *>(sender());
    if (!sock) return;
    dsv_info("MCP: client disconnected");
    if (_pending_capture.sock.data() == sock)
        _pending_capture = PendingCapture{};
    sock->deleteLater();
}

void McpServer::onClientReadyRead()
{
    QTcpSocket *sock = qobject_cast<QTcpSocket *>(sender());
    if (!sock) return;

    QByteArray buf = sock->property("_mcp_buf").toByteArray();
    buf.append(sock->readAll());

    int nl;
    while ((nl = buf.indexOf('\n')) >= 0) {
        QByteArray line = buf.left(nl);
        buf.remove(0, nl + 1);
        if (!line.trimmed().isEmpty())
            handleLine(sock, line);
    }
    sock->setProperty("_mcp_buf", buf);
}

void McpServer::handleLine(QTcpSocket *sock, const QByteArray &line)
{
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(line, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        writeError(sock, QJsonValue(), -32700,
                   QStringLiteral("Parse error: %1").arg(pe.errorString()));
        return;
    }
    QJsonObject req = doc.object();
    QJsonValue  id  = req.value("id");
    QString method  = req.value("method").toString();
    QJsonObject par = req.value("params").toObject();

    QString err;
    QJsonObject result;

    if (method == "ping") {
        result = toolPing(par, &err);
    } else if (method == "list_devices") {
        result = toolListDevices(par, &err);
    } else if (method == "device_info") {
        result = toolDeviceInfo(par, &err);
    } else if (method == "list_decoders") {
        result = toolListDecoders(par, &err);
    } else if (method == "capture") {
        if (!toolCaptureStart(sock, id, par, &err)) {
            writeError(sock, id, -32000, err);
        }
        /* Either we returned an error already, or the response will be
         * delivered later by onFrameEnded(). */
        return;
    } else if (method == "decode") {
        result = toolDecode(par, &err);
    } else {
        writeError(sock, id, -32601,
                   QStringLiteral("Method not found: %1").arg(method));
        return;
    }

    if (!err.isEmpty()) {
        writeError(sock, id, -32000, err);
    } else {
        writeResponse(sock, id, result);
    }
}

void McpServer::writeResponse(QTcpSocket *sock,
                              const QJsonValue &id,
                              const QJsonObject &result)
{
    if (!sock || sock->state() != QAbstractSocket::ConnectedState) return;
    QJsonObject env;
    env["jsonrpc"] = "2.0";
    env["id"] = id;
    env["result"] = result;
    QJsonDocument doc(env);
    sock->write(doc.toJson(QJsonDocument::Compact));
    sock->write("\n");
    sock->flush();
}

void McpServer::writeError(QTcpSocket *sock,
                           const QJsonValue &id,
                           int code,
                           const QString &message)
{
    if (!sock || sock->state() != QAbstractSocket::ConnectedState) return;
    QJsonObject env;
    env["jsonrpc"] = "2.0";
    env["id"] = id;
    QJsonObject err;
    err["code"] = code;
    err["message"] = message;
    env["error"] = err;
    QJsonDocument doc(env);
    sock->write(doc.toJson(QJsonDocument::Compact));
    sock->write("\n");
    sock->flush();
}

void McpServer::onFrameEnded()
{
    if (!_pending_capture.active) return;

    QPointer<QTcpSocket> sock = _pending_capture.sock;
    QJsonValue           id   = _pending_capture.id;
    _pending_capture = PendingCapture{};

    if (!sock || sock->state() != QAbstractSocket::ConnectedState) return;

    /* Build a brief status snapshot. The Python bridge will fetch raw
     * samples and channel info via separate tool calls if needed. */
    QJsonObject result;
    result["ok"] = true;

    if (_session) {
        result["samplerate"] = (qint64)_session->cur_snap_samplerate();
        result["samples"]    = (qint64)_session->cur_samplelimits();
        result["triggered"]  = _session->is_triged();
        result["trigger_pos"] = (qint64)_session->get_trigger_pos();
    }

    writeResponse(sock, id, result);
}

} // namespace mcp
} // namespace pv
