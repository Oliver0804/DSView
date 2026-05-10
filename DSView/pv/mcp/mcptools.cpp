/*
 * Tool implementations for the embedded MCP server. Runs on the GUI
 * thread, so it's safe to touch SigSession / DeviceAgent directly.
 */
#include "mcpserver.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <libsigrok.h>
#include <libsigrokdecode.h>
#include <glib.h>

#include "../sigsession.h"
#include "../deviceagent.h"
#include "../mainwindow.h"
#include "../log.h"

namespace pv {
namespace mcp {

static QString operation_mode_name(int m)
{
    switch (m) {
    case LOGIC: return "LOGIC";
    case DSO:   return "DSO";
    case ANALOG: return "ANALOG";
    default:    return "UNKNOWN";
    }
}

QJsonObject McpServer::toolPing(const QJsonObject &params, QString *err)
{
    Q_UNUSED(params); Q_UNUSED(err);
    QJsonObject o;
    o["pong"] = true;
    o["dsview"] = true;
    return o;
}

QJsonObject McpServer::toolListDevices(const QJsonObject &params, QString *err)
{
    Q_UNUSED(params); Q_UNUSED(err);

    /* Reload the libsigrok device list, then enumerate. */
    ds_reload_device_list();

    struct ds_device_base_info *arr = nullptr;
    int count = 0;
    QJsonArray devs;

    if (ds_get_device_list(&arr, &count) == SR_OK && arr) {
        for (int i = 0; i < count; i++) {
            QJsonObject d;
            d["index"] = i;
            d["handle"] = (qint64)arr[i].handle;
            d["name"]   = QString::fromUtf8(arr[i].name);
            devs.append(d);
        }
        g_free(arr);
    }

    QJsonObject result;
    result["devices"] = devs;

    /* Also report which device DSView currently has active. */
    if (_session && _session->get_device()) {
        DeviceAgent *agent = _session->get_device();
        QJsonObject act;
        act["handle"]    = (qint64)agent->handle();
        act["name"]      = agent->name();
        act["mode"]      = operation_mode_name(agent->get_work_mode());
        act["samplerate"] = (qint64)agent->get_sample_rate();
        act["depth"]     = (qint64)agent->get_sample_limit();
        result["active"] = act;
    }
    return result;
}

QJsonObject McpServer::toolDeviceInfo(const QJsonObject &params, QString *err)
{
    Q_UNUSED(params);

    if (!_session || !_session->get_device()) {
        if (err) *err = "no active device";
        return QJsonObject();
    }
    DeviceAgent *agent = _session->get_device();
    QJsonObject result;
    result["name"]       = agent->name();
    result["handle"]     = (qint64)agent->handle();
    result["mode"]       = operation_mode_name(agent->get_work_mode());
    result["samplerate"] = (qint64)agent->get_sample_rate();
    result["depth"]      = (qint64)agent->get_sample_limit();

    QJsonArray channels;
    GSList *chs = ds_get_actived_device_channels();
    for (GSList *l = chs; l; l = l->next) {
        const sr_channel *ch = (const sr_channel *)l->data;
        if (!ch) continue;
        QJsonObject c;
        c["index"]   = (int)ch->index;
        c["name"]    = ch->name ? QString::fromUtf8(ch->name) : QString();
        c["enabled"] = (bool)ch->enabled;
        c["type"]    = ch->type;
        channels.append(c);
    }
    result["channels"] = channels;
    return result;
}

QJsonObject McpServer::toolListDecoders(const QJsonObject &params,
                                        QString *err)
{
    QString filter = params.value("filter").toString().toLower();
    Q_UNUSED(err);

    QJsonArray decoders;
    for (const GSList *l = srd_decoder_list(); l; l = l->next) {
        const struct srd_decoder *d = (const struct srd_decoder *)l->data;
        if (!d) continue;

        QString id  = QString::fromUtf8(d->id ? d->id : "");
        QString lng = QString::fromUtf8(d->longname ? d->longname : "");
        if (!filter.isEmpty()
            && !id.toLower().contains(filter)
            && !lng.toLower().contains(filter))
            continue;

        QJsonObject obj;
        obj["id"]       = id;
        obj["name"]     = QString::fromUtf8(d->name ? d->name : "");
        obj["longname"] = lng;
        obj["desc"]     = QString::fromUtf8(d->desc ? d->desc : "");

        QJsonArray channels;
        for (GSList *cl = d->channels; cl; cl = cl->next) {
            const struct srd_channel *c = (const struct srd_channel *)cl->data;
            if (!c) continue;
            QJsonObject co;
            co["id"] = QString::fromUtf8(c->id ? c->id : "");
            co["name"] = QString::fromUtf8(c->name ? c->name : "");
            co["required"] = true;
            channels.append(co);
        }
        for (GSList *cl = d->opt_channels; cl; cl = cl->next) {
            const struct srd_channel *c = (const struct srd_channel *)cl->data;
            if (!c) continue;
            QJsonObject co;
            co["id"] = QString::fromUtf8(c->id ? c->id : "");
            co["name"] = QString::fromUtf8(c->name ? c->name : "");
            co["required"] = false;
            channels.append(co);
        }
        obj["channels"] = channels;

        QJsonArray options;
        for (GSList *ol = d->options; ol; ol = ol->next) {
            const struct srd_decoder_option *o =
                (const struct srd_decoder_option *)ol->data;
            if (!o) continue;
            QJsonObject oo;
            oo["id"] = QString::fromUtf8(o->id ? o->id : "");
            oo["desc"] = QString::fromUtf8(o->desc ? o->desc : "");
            options.append(oo);
        }
        obj["options"] = options;

        decoders.append(obj);
    }
    QJsonObject result;
    result["decoders"] = decoders;
    result["count"]    = decoders.size();
    return result;
}

bool McpServer::toolCaptureStart(QTcpSocket *sock, const QJsonValue &id,
                                 const QJsonObject &params, QString *err)
{
    if (!_session) { *err = "no SigSession"; return false; }
    if (_session->is_working()) {
        *err = "device is currently capturing";
        return false;
    }
    if (_pending_capture.active) {
        *err = "another capture call is in flight";
        return false;
    }

    /* Optional config overrides. */
    qint64 rate  = params.value("samplerate").toVariant().toLongLong();
    qint64 depth = params.value("depth").toVariant().toLongLong();

    DeviceAgent *agent = _session->get_device();
    if (!agent || agent->handle() == NULL_HANDLE) {
        *err = "no active device";
        return false;
    }

    if (rate > 0)
        ds_set_actived_device_config(NULL, NULL, SR_CONF_SAMPLERATE,
                                     g_variant_new_uint64((uint64_t)rate));
    if (depth > 0)
        ds_set_actived_device_config(NULL, NULL, SR_CONF_LIMIT_SAMPLES,
                                     g_variant_new_uint64((uint64_t)depth));

    if (params.contains("channels")) {
        QJsonArray chs = params.value("channels").toArray();
        /* First disable all logic channels. */
        GSList *all = ds_get_actived_device_channels();
        for (GSList *l = all; l; l = l->next) {
            sr_channel *c = (sr_channel *)l->data;
            if (c && c->type == SR_CHANNEL_LOGIC)
                ds_enable_device_channel(c, FALSE);
        }
        for (const QJsonValue &v : chs) {
            ds_enable_device_channel_index(v.toInt(), TRUE);
        }
    }

    /* SigSession::action_start_capture() will refresh cur_samplerate/limit
     * from the agent itself, so we don't need to set them here. */

    _pending_capture.sock   = sock;
    _pending_capture.id     = id;
    _pending_capture.active = true;

    if (!_session->start_capture(false)) {
        _pending_capture = PendingCapture{};
        *err = "start_capture() failed";
        return false;
    }
    /* Response is sent later from onFrameEnded(). */
    return true;
}

QJsonObject McpServer::toolDecode(const QJsonObject &params, QString *err)
{
    /* For v1 the GUI-embedded decode tool just signals "not yet
     * implemented in the embedded path"; the standalone helper still
     * works for richer decode output. The Python bridge can fall back to
     * the standalone helper if it wants. */
    Q_UNUSED(params);
    if (err)
        *err = "decode() not implemented in embedded MCP yet — "
               "fall back to the standalone helper";
    return QJsonObject();
}

} // namespace mcp
} // namespace pv
