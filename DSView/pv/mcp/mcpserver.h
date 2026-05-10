/*
 * Embedded MCP-style JSON-RPC server for DSView.
 * Listens on 127.0.0.1 only and dispatches tool calls to the running
 * SigSession on the GUI thread.
 *
 * Wire format: newline-delimited JSON-RPC 2.0.
 *   request:  {"jsonrpc":"2.0","id":<n>,"method":"<m>","params":{...}}
 *   response: {"jsonrpc":"2.0","id":<n>,"result":{...}}    or
 *             {"jsonrpc":"2.0","id":<n>,"error":{"code":<c>,"message":"..."}}
 *
 * The translation between MCP (stdio) and this protocol is done by a
 * Python bridge (see mcp/dsview_mcp/server.py).
 */
#ifndef _PV_MCP_MCPSERVER_H
#define _PV_MCP_MCPSERVER_H

#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>

namespace pv {
class SigSession;
class MainWindow;

namespace mcp {

class McpServer : public QObject
{
    Q_OBJECT

public:
    McpServer(SigSession *session, MainWindow *main_window,
              QObject *parent = nullptr);
    ~McpServer();

    bool start(quint16 port);
    void stop();

    quint16 listenPort() const { return _server.serverPort(); }
    bool isListening() const { return _server.isListening(); }

signals:
    /* Emitted for every log-worthy event: server start/stop, client
     * connect/disconnect, request received, response/error sent.
     * Already prefixed with a "[hh:mm:ss]" timestamp. The log dialog
     * (or any other observer) can listen and append. */
    void logMessage(const QString &line);

private slots:
    void onNewConnection();
    void onClientReadyRead();
    void onClientDisconnected();

    /* Capture lifecycle hooks (connected to MainWindow's EventObject). */
    void onFrameEnded();

private:
    void handleLine(QTcpSocket *sock, const QByteArray &line);
    void log(const QString &text);
    void writeResponse(QTcpSocket *sock,
                       const QJsonValue &id,
                       const QJsonObject &result);
    void writeError(QTcpSocket *sock,
                    const QJsonValue &id,
                    int code,
                    const QString &message);

    /* tool implementations live in mcptools.cpp */
    QJsonObject toolPing(const QJsonObject &params, QString *err);
    QJsonObject toolListDevices(const QJsonObject &params, QString *err);
    QJsonObject toolDeviceInfo(const QJsonObject &params, QString *err);
    QJsonObject toolListDecoders(const QJsonObject &params, QString *err);

    /* capture is async — initiates and returns false; response is sent
     * later from onFrameEnded. */
    bool toolCaptureStart(QTcpSocket *sock, const QJsonValue &id,
                          const QJsonObject &params, QString *err);

    QJsonObject toolDecode(const QJsonObject &params, QString *err);
    QJsonObject toolScreenshot(const QJsonObject &params, QString *err);

    /* GUI control surface — equivalent to clicking buttons / dropdowns
     * on the main toolbar. All sync; do not block on a frame. */
    QJsonObject toolRunStart(const QJsonObject &params, QString *err);
    QJsonObject toolRunStop(const QJsonObject &params, QString *err);
    QJsonObject toolInstantShot(const QJsonObject &params, QString *err);
    QJsonObject toolSetActiveDevice(const QJsonObject &params, QString *err);
    QJsonObject toolSetConfig(const QJsonObject &params, QString *err);
    QJsonObject toolSetChannel(const QJsonObject &params, QString *err);
    QJsonObject toolSetCollectMode(const QJsonObject &params, QString *err);
    QJsonObject toolGetState(const QJsonObject &params, QString *err);
    QJsonObject toolSaveSession(const QJsonObject &params, QString *err);
    QJsonObject toolLoadSession(const QJsonObject &params, QString *err);

private:
    QTcpServer            _server;
    SigSession           *_session;
    QPointer<MainWindow>  _main_window;

    /* The single in-flight capture call (if any). */
    struct PendingCapture {
        QPointer<QTcpSocket> sock;
        QJsonValue           id;
        bool                 active = false;
    } _pending_capture;
};

} // namespace mcp
} // namespace pv

#endif
