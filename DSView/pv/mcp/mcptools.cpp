/*
 * Tool implementations for the embedded MCP server. Runs on the GUI
 * thread, so it's safe to touch SigSession / DeviceAgent directly.
 */
#include "mcpserver.h"

#include <QBuffer>
#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QPixmap>
#include <QString>

#include <libsigrok.h>
#include <libsigrokdecode.h>
#include <glib.h>

#include "../sigsession.h"
#include "../deviceagent.h"
#include "../mainwindow.h"
#include "../toolbars/samplingbar.h"
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

/* ---- shared helpers for set_config / capture-with-options ----------- */

namespace {

struct ConfigKeyDesc {
    const char *name;
    int         key;
    char        type;   /* 'd'=double, 'i'=int16, 'b'=bool, 's'=string,
                         * 'u'=uint64 */
};

static const ConfigKeyDesc CONFIG_KEYS[] = {
    {"vth",                SR_CONF_VTH,             'd'},
    {"operation_mode",     SR_CONF_OPERATION_MODE,  'i'},
    {"buffer_option",      SR_CONF_BUFFER_OPTIONS,  'i'},
    {"filter",             SR_CONF_FILTER,          'i'},
    {"channel_mode",       SR_CONF_CHANNEL_MODE,    'i'},
    {"rle",                SR_CONF_RLE,             'b'},
    {"ext_clock",          SR_CONF_CLOCK_TYPE,      'b'},
    {"falling_edge_clock", SR_CONF_CLOCK_EDGE,      'b'},
    {"max_height",         SR_CONF_MAX_HEIGHT,      's'},
    {"samplerate",         SR_CONF_SAMPLERATE,      'u'},
    {"depth",              SR_CONF_LIMIT_SAMPLES,   'u'},
    /* virtual-demo: pick the pattern source. "random" = pseudo-random,
     * any other string is the basename of a .demo file under
     * ~/.dsview/demo/logic (e.g. "protocol"). Has no effect on real
     * DSLogic / DSCope devices. */
    {"pattern_mode",       SR_CONF_PATTERN_MODE,    's'},
};

static const ConfigKeyDesc *config_lookup(const QString &name) {
    for (const auto &d : CONFIG_KEYS)
        if (name == QLatin1String(d.name)) return &d;
    return nullptr;
}

/* After tweaking driver-level config, ask the toolbar widgets to re-read
 * the device state so their dropdowns reflect what changed. The various
 * sampling_bar slots are private but update_sample_rate_list / device
 * list are public. */
static void refresh_toolbar_from_device(MainWindow *mw)
{
    if (!mw) return;
    auto *sb = mw->getSamplingBar();
    if (!sb) return;
    sb->update_sample_rate_list();
    sb->update_view_status();
}

/* Apply one (key, value) pair against the active device. Returns "" on
 * success, otherwise an error message. */
static QString apply_config(const QString &name, const QJsonValue &val)
{
    const ConfigKeyDesc *d = config_lookup(name);
    if (!d) return QStringLiteral("unknown key: %1").arg(name);

    GVariant *gv = nullptr;
    switch (d->type) {
    case 'd': gv = g_variant_new_double(val.toDouble()); break;
    case 'i': gv = g_variant_new_int16((int16_t)val.toInt()); break;
    case 'u': gv = g_variant_new_uint64(
                  (uint64_t)val.toVariant().toLongLong()); break;
    case 'b': gv = g_variant_new_boolean(val.toBool() ? TRUE : FALSE); break;
    case 's': gv = g_variant_new_string(val.toString().toUtf8().constData());
              break;
    default: return QStringLiteral("internal: bad type for %1").arg(name);
    }
    int rc = ds_set_actived_device_config(NULL, NULL, d->key, gv);
    if (rc != SR_OK)
        return QStringLiteral("ds_set_actived_device_config(%1) -> %2")
            .arg(name).arg(sr_error_str(rc));
    return QString();
}

} // anonymous

/* Grab the DSView main window and write it as PNG to a path the LLM can
 * read with its file/Image tooling. Returning the bytes directly would
 * blow up the JSON-RPC channel (a 1920×1170 PNG is ~400KB / ~530KB
 * base64 / ~130k tokens), so we write to disk and report the path. */
QJsonObject McpServer::toolScreenshot(const QJsonObject &params, QString *err)
{
    if (!_main_window) {
        if (err) *err = "main window unavailable";
        return QJsonObject();
    }

    QString path = params.value("path").toString();
    if (path.isEmpty()) {
        path = QDir::tempPath()
             + QStringLiteral("/dsview_screenshot_%1.png")
                .arg(QDateTime::currentMSecsSinceEpoch());
    }

    QPixmap pix = _main_window->grab();
    if (pix.isNull()) {
        if (err) *err = "QWidget::grab() returned null";
        return QJsonObject();
    }

    /* Optional downscale to keep the file vision-friendly. */
    int max_w = params.value("max_width").toInt(0);
    if (max_w > 0 && pix.width() > max_w) {
        pix = pix.scaledToWidth(max_w, Qt::SmoothTransformation);
    }

    int quality = params.value("quality").toInt(-1);
    Q_UNUSED(quality); /* PNG ignores quality, kept for API symmetry. */

    if (!pix.save(path, "PNG")) {
        if (err) *err = QStringLiteral("failed to save PNG to %1").arg(path);
        return QJsonObject();
    }

    QFileInfo fi(path);
    QJsonObject r;
    r["path"]   = fi.absoluteFilePath();
    r["width"]  = pix.width();
    r["height"] = pix.height();
    r["bytes"]  = (qint64)fi.size();
    return r;
}

/* ---- toolbar buttons ------------------------------------------------ */

QJsonObject McpServer::toolRunStart(const QJsonObject &params, QString *err)
{
    Q_UNUSED(params);
    if (!_session) { if (err) *err = "no SigSession"; return {}; }
    if (_session->is_working()) {
        if (err) *err = "already capturing";
        return {};
    }
    if (!_session->start_capture(false)) {
        if (err) *err = "start_capture(false) failed";
        return {};
    }
    QJsonObject r; r["ok"] = true; r["action"] = "run_start"; return r;
}

QJsonObject McpServer::toolRunStop(const QJsonObject &params, QString *err)
{
    Q_UNUSED(params); Q_UNUSED(err);
    if (!_session) { if (err) *err = "no SigSession"; return {}; }
    QJsonObject r; r["action"] = "run_stop";
    if (!_session->is_working()) {
        r["ok"] = true;
        r["already_stopped"] = true;
        return r;
    }
    bool ok = _session->stop_capture();
    r["ok"] = ok;
    if (!ok && err) *err = "stop_capture() failed";
    return r;
}

QJsonObject McpServer::toolInstantShot(const QJsonObject &params, QString *err)
{
    Q_UNUSED(params);
    if (!_session) { if (err) *err = "no SigSession"; return {}; }
    if (_session->is_working()) {
        if (err) *err = "already capturing";
        return {};
    }
    if (!_session->start_capture(true)) {
        if (err) *err = "start_capture(true) failed";
        return {};
    }
    QJsonObject r; r["ok"] = true; r["action"] = "instant_shot"; return r;
}

QJsonObject McpServer::toolSetActiveDevice(const QJsonObject &params,
                                           QString *err)
{
    if (!_session) { if (err) *err = "no SigSession"; return {}; }
    if (_session->is_working()) {
        if (err) *err = "device is currently capturing; call run_stop "
                        "first before switching active device";
        return {};
    }

    /* Accept either {index} or {handle}. */
    qint64 want_handle = params.value("handle").toVariant().toLongLong();
    int    idx         = params.value("index").toInt(-1);

    if (want_handle == 0) {
        if (idx < 0) {
            if (err) *err = "params.index or params.handle required";
            return {};
        }
        struct ds_device_base_info *arr = nullptr;
        int count = 0;
        ds_reload_device_list();
        if (ds_get_device_list(&arr, &count) != SR_OK || !arr || idx >= count) {
            if (arr) g_free(arr);
            if (err) *err = QStringLiteral("index %1 out of range").arg(idx);
            return {};
        }
        want_handle = (qint64)arr[idx].handle;
        g_free(arr);
    }

    /* SigSession::set_device() does the full GUI-side switch: releases the
     * old DeviceAgent, calls ds_active_device(handle), refreshes the agent,
     * resets collect mode, and emits the signals the toolbar listens for. */
    if (!_session->set_device((ds_device_handle)want_handle)) {
        if (err) *err = "SigSession::set_device() failed";
        return {};
    }

    /* Make the device dropdown / samplerate / depth selectors mirror the
     * new active device. */
    if (_main_window && _main_window->getSamplingBar()) {
        auto *sb = _main_window->getSamplingBar();
        sb->update_device_list();
        sb->update_sample_rate_list();
        sb->update_view_status();
    }

    QJsonObject r;
    r["ok"]     = true;
    r["index"]  = idx;
    r["handle"] = want_handle;
    if (_session->get_device())
        r["name"] = _session->get_device()->name();
    return r;
}

/* set_config({key: value, key: value, ...}) — sets one or more device
 * options in a single call. Reports per-key results. */
QJsonObject McpServer::toolSetConfig(const QJsonObject &params, QString *err)
{
    Q_UNUSED(err);
    QJsonObject applied, errors;
    for (auto it = params.constBegin(); it != params.constEnd(); ++it) {
        QString e = apply_config(it.key(), it.value());
        if (e.isEmpty()) applied[it.key()] = it.value();
        else            errors[it.key()] = e;
    }
    if (!applied.isEmpty()) refresh_toolbar_from_device(_main_window);
    QJsonObject r;
    r["applied"] = applied;
    if (!errors.isEmpty()) r["errors"] = errors;
    r["ok"] = errors.isEmpty();
    return r;
}

QJsonObject McpServer::toolSetChannel(const QJsonObject &params, QString *err)
{
    /* Two shapes:
     *   {index: int, enabled: bool}            — single channel
     *   {channels: {"0": true, "1": false...}} — batch */
    QJsonObject applied;
    if (params.contains("index")) {
        int  idx = params.value("index").toInt(-1);
        bool en  = params.value("enabled").toBool(true);
        if (idx < 0) { if (err) *err = "params.index required"; return {}; }
        ds_enable_device_channel_index(idx, en ? TRUE : FALSE);
        applied[QString::number(idx)] = en;
    }
    QJsonObject batch = params.value("channels").toObject();
    for (auto it = batch.constBegin(); it != batch.constEnd(); ++it) {
        bool en = it.value().toBool(true);
        ds_enable_device_channel_index(it.key().toInt(), en ? TRUE : FALSE);
        applied[it.key()] = en;
    }
    refresh_toolbar_from_device(_main_window);
    QJsonObject r; r["ok"] = true; r["channels"] = applied; return r;
}

QJsonObject McpServer::toolSetCollectMode(const QJsonObject &params,
                                          QString *err)
{
    if (!_session) { if (err) *err = "no SigSession"; return {}; }

    /* SigSession::set_collect_mode() asserts !_is_working — calling it
     * mid-capture would SIGABRT the entire GUI. Refuse early instead. */
    if (_session->is_working()) {
        if (err) *err = "device is currently capturing; call run_stop "
                        "first before changing collect mode";
        return {};
    }

    QString m = params.value("mode").toString().toLower();
    DEVICE_COLLECT_MODE cm;
    if (m == "single")        cm = COLLECT_SINGLE;
    else if (m == "repeat")   cm = COLLECT_REPEAT;
    else if (m == "loop")     cm = COLLECT_LOOP;
    else { if (err) *err = "mode must be single|repeat|loop"; return {}; }
    _session->set_collect_mode(cm);
    QJsonObject r; r["ok"] = true; r["mode"] = m; return r;
}

QJsonObject McpServer::toolGetState(const QJsonObject &params, QString *err)
{
    Q_UNUSED(params); Q_UNUSED(err);
    QJsonObject r;
    if (!_session) { r["ok"] = false; r["reason"] = "no session"; return r; }

    DeviceAgent *agent = _session->get_device();
    if (agent && agent->handle() != NULL_HANDLE) {
        QJsonObject d;
        d["name"]       = agent->name();
        d["mode"]       = operation_mode_name(agent->get_work_mode());
        d["samplerate"] = (qint64)agent->get_sample_rate();
        d["depth"]      = (qint64)agent->get_sample_limit();
        r["device"] = d;
    }
    r["working"]      = _session->is_working();
    r["instant"]      = _session->is_instant();
    r["collect_mode"] = _session->is_loop_mode()    ? "loop"
                       : _session->is_repeat_mode() ? "repeat"
                       : "single";
    r["mcp_listening"] = _server.isListening();
    r["mcp_port"]      = _server.serverPort();
    r["ok"] = true;
    return r;
}

} // namespace mcp
} // namespace pv
