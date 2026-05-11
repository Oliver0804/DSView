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
#include "../eventobject.h"
#include "../mainwindow.h"
#include "../storesession.h"
#include "../toolbars/samplingbar.h"
#include "../view/view.h"
#include "../view/decodetrace.h"
#include "../data/decoderstack.h"
#include "../data/decode/decoder.h"
#include "../data/decode/annotation.h"
#include "../data/decode/row.h"
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

/* save_session({path?: string}) — write the current capture (samples +
 * channel layout + decoder stack) to a .dsl file. If `path` is omitted
 * we auto-name into the user's home dir, the same naming scheme the
 * GUI's "Save" button uses. Blocks until the writer thread finishes. */
QJsonObject McpServer::toolSaveSession(const QJsonObject &params,
                                       QString *err)
{
    if (!_session) { if (err) *err = "no SigSession"; return {}; }
    if (_session->is_working()) {
        if (err) *err = "device is currently capturing; call run_stop "
                        "first before saving";
        return {};
    }

    /* Same precondition the GUI's on_save() enforces. */
    bool have_data = false;
    for (auto s : _session->get_signals()) {
        Q_UNUSED(s);
        have_data = true;
        break;
    }
    if (!have_data) {
        if (err) *err = "no captured data to save (run a capture first)";
        return {};
    }

    StoreSession ss(_session);

    QString path = params.value("path").toString();
    if (path.isEmpty()) {
        /* Auto-name: identical to MakeSaveFile(false) but stash result. */
        path = ss.MakeSaveFile(false);
    } else {
        if (!path.endsWith(".dsl", Qt::CaseInsensitive))
            path += ".dsl";
        ss.SetFileName(path);
    }

    /* MainWindow implements ISessionDataGetter; the writer needs it for
     * decoder annotations and view-bound metadata. */
    if (_main_window)
        ss.SetSessionDataGetter(_main_window);

    _session->set_saving(true);
    bool ok = ss.save_start();
    if (ok) ss.wait();
    _session->set_saving(false);

    QJsonObject r;
    r["ok"]   = ok;
    r["path"] = path;
    if (ok) {
        QFileInfo fi(path);
        r["bytes"] = (qint64)fi.size();
    } else {
        if (err) *err = ss.error();
    }
    return r;
}

/* set_channel_name({index: int, name: string}) — rename a logic
 * channel so subsequent device_info / capture metadata / decoder
 * auto-mapping see the user-friendly label. Mutates sr_channel::name
 * directly and asks the GUI to redraw the trace label. */
QJsonObject McpServer::toolSetChannelName(const QJsonObject &params,
                                          QString *err)
{
    if (!_session) { if (err) *err = "no SigSession"; return {}; }
    int idx = params.value("index").toInt(-1);
    QString new_name = params.value("name").toString();
    if (idx < 0) { if (err) *err = "params.index required"; return {}; }
    if (new_name.isEmpty()) {
        if (err) *err = "params.name required (non-empty string)";
        return {};
    }

    sr_channel *target = nullptr;
    GSList *chs = ds_get_actived_device_channels();
    for (GSList *l = chs; l; l = l->next) {
        sr_channel *c = (sr_channel *)l->data;
        if (c && c->index == idx) { target = c; break; }
    }
    if (!target) {
        if (err) *err = QStringLiteral("channel index %1 not found").arg(idx);
        return {};
    }

    if (target->name) g_free(target->name);
    target->name = g_strdup(new_name.toUtf8().constData());

    /* Tell views to repaint with the new label. SigSession::signals_changed
     * is non-public, so go through MainWindow's EventObject instead. */
    if (_main_window && _main_window->getEvent())
        emit _main_window->getEvent()->signals_changed();

    QJsonObject r;
    r["ok"]    = true;
    r["index"] = idx;
    r["name"]  = new_name;
    return r;
}

/* load_session({path: string}) — open a previously saved .dsl file in
 * the GUI. Counterpart to save_session. */
QJsonObject McpServer::toolLoadSession(const QJsonObject &params,
                                       QString *err)
{
    if (!_session) { if (err) *err = "no SigSession"; return {}; }
    if (_session->is_working()) {
        if (err) *err = "device is currently capturing; call run_stop "
                        "first before loading a session";
        return {};
    }
    QString path = params.value("path").toString();
    if (path.isEmpty()) {
        if (err) *err = "params.path required (.dsl file to open)";
        return {};
    }
    if (!QFileInfo(path).isFile()) {
        if (err) *err = QStringLiteral("file not found: %1").arg(path);
        return {};
    }

    /* SigSession::set_file() throws QString on failure — catch and
     * surface as JSON-RPC error. */
    try {
        if (!_session->set_file(path)) {
            if (err) *err = "set_file() returned false";
            return {};
        }
    } catch (QString e) {
        if (err) *err = QStringLiteral("set_file failed: %1").arg(e);
        return {};
    }

    /* Refresh toolbar to mirror the loaded session's settings. */
    if (_main_window && _main_window->getSamplingBar()) {
        auto *sb = _main_window->getSamplingBar();
        sb->update_device_list();
        sb->update_sample_rate_list();
        sb->update_view_status();
    }

    QJsonObject r;
    r["ok"]   = true;
    r["path"] = path;
    if (_session->get_device())
        r["device"] = _session->get_device()->name();
    return r;
}

/* ---- live-GUI decoder control --------------------------------------- */

namespace {

/* DSView ships decoders with "<bus#>:<protocol>" ids (e.g. "1:i2c",
 * "0:i2c", "1:spi") — the prefix selects which bus the decoder reads
 * its data from. If the caller passes the bare protocol ("i2c"), try
 * "1:" first (single-bus, the common case) then "0:" before giving up.
 * Stack ids ("eeprom24xx") have no prefix, so an exact match is fine. */
static srd_decoder *find_srd_decoder_by_id_raw(const QString &id)
{
    QByteArray idbytes = id.toUtf8();
    for (const GSList *l = srd_decoder_list(); l; l = l->next) {
        srd_decoder *d = (srd_decoder *)l->data;
        if (d && d->id && !strcmp(d->id, idbytes.constData()))
            return d;
    }
    return NULL;
}

static srd_decoder *find_srd_decoder_by_id(const QString &id)
{
    /* Try the user's id verbatim. */
    if (auto *d = find_srd_decoder_by_id_raw(id)) return d;
    /* Caller passed a bare protocol — try the bus-prefixed variants. */
    if (!id.contains(':')) {
        if (auto *d = find_srd_decoder_by_id_raw("1:" + id)) return d;
        if (auto *d = find_srd_decoder_by_id_raw("0:" + id)) return d;
    }
    return NULL;
}

static const srd_channel *find_channel_by_id(const srd_decoder *dec,
                                             const char *id)
{
    for (GSList *l = dec->channels; l; l = l->next) {
        const srd_channel *c = (const srd_channel *)l->data;
        if (c && c->id && !strcmp(c->id, id)) return c;
    }
    for (GSList *l = dec->opt_channels; l; l = l->next) {
        const srd_channel *c = (const srd_channel *)l->data;
        if (c && c->id && !strcmp(c->id, id)) return c;
    }
    return NULL;
}

/* Convert a JSON value to a freshly-ref'd GVariant. Type is inferred
 * from the JSON shape — libsigrokdecode coerces from common types so
 * "address_format": "shifted" works without the caller knowing the
 * decoder's preferred GVariant type. */
static GVariant *jsonvalue_to_gvariant(const QJsonValue &v)
{
    if (v.isBool())   return g_variant_new_boolean(v.toBool() ? TRUE : FALSE);
    if (v.isString()) return g_variant_new_string(v.toString().toUtf8().constData());
    if (v.isDouble()) {
        double d = v.toDouble();
        if (d == (qint64)d)
            return g_variant_new_int64((qint64)d);
        return g_variant_new_double(d);
    }
    return NULL;
}

/* Apply channel + option maps to a single Decoder. Returns "" on success. */
static QString apply_decoder_config(pv::data::decode::Decoder *dec_obj,
                                    const srd_decoder *srd_dec,
                                    const QJsonObject &channels,
                                    const QJsonObject &options)
{
    std::map<const srd_channel *, int> probes;
    for (auto it = channels.constBegin(); it != channels.constEnd(); ++it) {
        const srd_channel *ch = find_channel_by_id(
            srd_dec, it.key().toUtf8().constData());
        if (!ch) {
            return QStringLiteral("decoder '%1' has no channel '%2'")
                .arg(srd_dec->id).arg(it.key());
        }
        probes[ch] = it.value().toInt();
    }
    dec_obj->set_probes(probes);

    for (auto it = options.constBegin(); it != options.constEnd(); ++it) {
        GVariant *gv = jsonvalue_to_gvariant(it.value());
        if (!gv) {
            return QStringLiteral("option '%1' has unsupported value type")
                .arg(it.key());
        }
        dec_obj->set_option(it.key().toUtf8().constData(), gv);
    }
    return QString();
}

} // anonymous

/* gui_add_decoder({protocol, channels, options?, stack?}) — add a
 * decoder to the running GUI so annotations show up on the wave view.
 *   - protocol: base decoder id (e.g. "i2c", "spi", or legacy "1:i2c")
 *   - channels: {<srd channel id>: <wire index>} — required channels
 *     must be present, optional channels may be omitted
 *   - options:  {<srd option id>: <value>} optional
 *   - stack:    [{protocol, channels?, options?}, ...] upper-layer
 *               decoders stacked on top (e.g. eeprom24xx on i2c)
 */
QJsonObject McpServer::toolGuiAddDecoder(const QJsonObject &params,
                                         QString *err)
{
    if (!_session) { if (err) *err = "no SigSession"; return {}; }
    if (_session->is_working()) {
        if (err) *err = "device is currently capturing; call run_stop first";
        return {};
    }

    QString proto = params.value("protocol").toString();
    if (proto.isEmpty()) {
        if (err) *err = "params.protocol required (decoder id)";
        return {};
    }
    srd_decoder *base = find_srd_decoder_by_id(proto);
    if (!base) {
        if (err) *err = QStringLiteral("decoder '%1' not found "
                                       "(see list_decoders)").arg(proto);
        return {};
    }

    /* Build the sub_decoder list first — these Decoder objects will be
     * adopted into the new stack by SigSession::add_decoder. */
    std::list<pv::data::decode::Decoder *> sub_decoders;
    QJsonArray stack = params.value("stack").toArray();
    QStringList stack_ids;
    for (const QJsonValue &sv : stack) {
        QJsonObject s = sv.toObject();
        QString sid = s.value("protocol").toString();
        srd_decoder *sdec = find_srd_decoder_by_id(sid);
        if (!sdec) {
            for (auto *d : sub_decoders) delete d;
            if (err) *err = QStringLiteral("stacked decoder '%1' not found")
                                .arg(sid);
            return {};
        }
        auto *dobj = new pv::data::decode::Decoder(sdec);
        QString cerr = apply_decoder_config(
            dobj, sdec,
            s.value("channels").toObject(),
            s.value("options").toObject());
        if (!cerr.isEmpty()) {
            for (auto *d : sub_decoders) delete d;
            delete dobj;
            if (err) *err = cerr;
            return {};
        }
        sub_decoders.push_back(dobj);
        stack_ids.append(sid);
    }

    DecoderStatus *dstatus = new DecoderStatus();
    dstatus->m_format = (int)DecoderDataFormat::hex;
    pv::view::Trace *trace_out = nullptr;

    /* silent=true — skip the channel-config popup. We set probes
     * ourselves below. */
    if (!_session->add_decoder(base, true, dstatus, sub_decoders, trace_out)) {
        delete dstatus;
        if (err) *err = "SigSession::add_decoder() failed";
        return {};
    }

    /* The trace returned points at the newly-created DecodeTrace whose
     * stack().front() is the base Decoder. Configure it now. */
    auto &traces = _session->get_decode_signals();
    int index = (int)traces.size() - 1;
    if (index < 0) {
        if (err) *err = "decoder added but trace not found";
        return {};
    }
    auto *trace = traces[index];
    auto *stack_obj = trace->decoder();
    auto *base_dec = stack_obj->stack().front();
    QString cerr = apply_decoder_config(
        base_dec, base,
        params.value("channels").toObject(),
        params.value("options").toObject());
    if (!cerr.isEmpty()) {
        _session->remove_decoder(index);
        if (err) *err = cerr;
        return {};
    }

    /* Kick off the decode worker so annotations actually populate.
     * restart_decoder() refuses while the device is mid-capture or
     * the view has no data — surface that so the caller can retry. */
    bool decode_started = _session->restart_decoder(index);

    /* Make sure the protocol dock + view repaint with the new trace. */
    if (_main_window && _main_window->getEvent())
        emit _main_window->getEvent()->signals_changed();

    QJsonObject r;
    r["ok"]         = true;
    r["index"]      = index;
    r["protocol"]   = QString::fromUtf8(base->id);  // echo resolved id
    r["stack"]      = QJsonArray::fromStringList(stack_ids);
    r["channels"]   = params.value("channels").toObject();
    r["options"]    = params.value("options").toObject();
    r["decode_started"] = decode_started;  // false = call gui_restart_decoder later
    return r;
}

QJsonObject McpServer::toolGuiRestartDecoder(const QJsonObject &params,
                                             QString *err)
{
    if (!_session) { if (err) *err = "no SigSession"; return {}; }
    int idx = params.value("index").toInt(-1);
    auto &traces = _session->get_decode_signals();
    if (idx < 0 || idx >= (int)traces.size()) {
        if (err) *err = QStringLiteral("index %1 out of range").arg(idx);
        return {};
    }
    bool ok = _session->restart_decoder(idx);
    QJsonObject r;
    r["ok"]    = ok;
    r["index"] = idx;
    if (!ok)
        r["reason"] = _session->is_working()
            ? "device is currently capturing"
            : "no captured data — run instant_shot / capture first";
    return r;
}

QJsonObject McpServer::toolGuiListDecoders(const QJsonObject &params,
                                           QString *err)
{
    Q_UNUSED(params); Q_UNUSED(err);
    if (!_session) { if (err) *err = "no SigSession"; return {}; }

    QJsonArray arr;
    auto &traces = _session->get_decode_signals();
    int idx = 0;
    for (auto *trace : traces) {
        QJsonObject o;
        o["index"] = idx++;
        auto *stack_obj = trace->decoder();
        QJsonArray stack_ids;
        for (auto *d : stack_obj->stack()) {
            if (d && d->decoder())
                stack_ids.append(QString::fromUtf8(d->decoder()->id));
        }
        if (!stack_ids.isEmpty()) {
            o["protocol"] = stack_ids.first().toString();
            QJsonArray rest;
            for (int i = 1; i < stack_ids.size(); ++i)
                rest.append(stack_ids[i]);
            o["stack"] = rest;
        }
        o["running"] = stack_obj->IsRunning();
        o["samples_decoded"] = (qint64)stack_obj->samples_decoded();
        arr.append(o);
    }
    QJsonObject r;
    r["ok"]       = true;
    r["count"]    = arr.size();
    r["decoders"] = arr;
    return r;
}

QJsonObject McpServer::toolGuiRemoveDecoder(const QJsonObject &params,
                                            QString *err)
{
    if (!_session) { if (err) *err = "no SigSession"; return {}; }
    int idx = params.value("index").toInt(-1);
    auto &traces = _session->get_decode_signals();
    if (idx < 0 || idx >= (int)traces.size()) {
        if (err) *err = QStringLiteral("index %1 out of range (0..%2)")
            .arg(idx).arg((int)traces.size() - 1);
        return {};
    }
    _session->remove_decoder(idx);
    if (_main_window && _main_window->getEvent())
        emit _main_window->getEvent()->signals_changed();
    QJsonObject r;
    r["ok"] = true;
    r["index"] = idx;
    return r;
}

QJsonObject McpServer::toolGuiClearDecoders(const QJsonObject &params,
                                            QString *err)
{
    Q_UNUSED(params); Q_UNUSED(err);
    if (!_session) { if (err) *err = "no SigSession"; return {}; }
    int removed = (int)_session->get_decode_signals().size();
    _session->clear_all_decoder(true);
    QJsonObject r;
    r["ok"]      = true;
    r["removed"] = removed;
    return r;
}

/* gui_decoder_annotations({index, row?, start?, end?, limit?, format?})
 * — extract decoded annotations from a GUI decoder so the LLM can read
 * the bytes/events without dumping the entire raw buffer.
 *   - index:  which trace (from gui_list_decoders)
 *   - row:    optional row index filter (-1 / omitted = all rows)
 *   - start/end: sample range filter (default = whole capture)
 *   - limit:  truncate after N events (default 1000, max 20000)
 *   - format: "list"    full dict per event (most readable)
 *             "summary" RLE-collapsed adjacent same-text events
 *             "compact" array-of-arrays + row legend (≈3× cheaper in
 *                       tokens than "list" / "summary" — recommended
 *                       once you know which fields you need)
 */
QJsonObject McpServer::toolGuiDecoderAnnotations(const QJsonObject &params,
                                                 QString *err)
{
    if (!_session) { if (err) *err = "no SigSession"; return {}; }
    int idx = params.value("index").toInt(-1);
    auto &traces = _session->get_decode_signals();
    if (idx < 0 || idx >= (int)traces.size()) {
        if (err) *err = QStringLiteral("index %1 out of range (0..%2)")
            .arg(idx).arg((int)traces.size() - 1);
        return {};
    }
    auto *stack_obj = traces[idx]->decoder();

    int     row_filter = params.value("row").toInt(-1);
    qint64  start_s    = params.value("start").toVariant().toLongLong();
    qint64  end_s      = params.value("end").toVariant().toLongLong();
    int     limit      = params.value("limit").toInt(1000);
    if (limit <= 0 || limit > 20000) limit = 1000;
    QString fmt        = params.value("format").toString("list");

    /* Walk the per-row annotation tables. DecoderStack exposes
     * list_annotation(row, col) for index-based row access; size via
     * list_annotation_size(row_index). */
    QJsonArray events;
    qint64 total = 0;
    bool truncated = false;

    auto rows_gshow = stack_obj->get_rows_gshow();
    int row_index = 0;
    for (auto it = rows_gshow.begin(); it != rows_gshow.end(); ++it, ++row_index) {
        if (row_filter >= 0 && row_filter != row_index) continue;
        const pv::data::decode::Row &row = it->first;
        QString row_title = row.title();
        uint64_t count = stack_obj->list_annotation_size(row_index);
        for (uint64_t i = 0; i < count; ++i) {
            pv::data::decode::Annotation ann;
            if (!stack_obj->list_annotation(&ann, row_index, i)) continue;
            qint64 s = (qint64)ann.start_sample();
            qint64 e = (qint64)ann.end_sample();
            if (end_s > 0 && s > end_s) continue;
            if (start_s > 0 && e < start_s) continue;
            ++total;
            if ((int)events.size() >= limit) {
                truncated = true;
                continue;
            }
            const auto &texts = ann.annotations();
            QJsonObject ev;
            ev["row"]   = row_index;
            ev["row_title"] = row_title;
            ev["start"] = s;
            ev["end"]   = e;
            if (!texts.empty()) ev["text"] = texts.front();
            events.append(ev);
        }
    }

    /* RLE-collapse adjacent identical-text rows when summary mode. */
    if ((fmt == "summary" || fmt == "compact") && events.size() > 1) {
        QJsonArray collapsed;
        QJsonObject cur = events.first().toObject();
        int run = 1;
        for (int i = 1; i < events.size(); ++i) {
            QJsonObject e = events[i].toObject();
            if (e["row"] == cur["row"] && e["text"] == cur["text"]) {
                cur["end"] = e["end"]; ++run;
            } else {
                if (run > 1) cur["run"] = run;
                collapsed.append(cur);
                cur = e; run = 1;
            }
        }
        if (run > 1) cur["run"] = run;
        collapsed.append(cur);
        events = collapsed;
    }

    QJsonObject r;
    r["ok"]        = true;
    r["index"]     = idx;
    r["format"]    = fmt;
    r["count"]     = (qint64)events.size();
    r["total"]     = total;
    r["truncated"] = truncated;

    if (fmt == "compact") {
        /* Array-of-arrays + row legend. Slashes the per-event byte
         * count from ~100 → ~35 by dropping repeating key names and
         * row titles. */
        QJsonObject row_legend;
        QJsonArray  tight;
        for (const QJsonValue &v : events) {
            QJsonObject e = v.toObject();
            QString rk = QString::number(e["row"].toInt());
            if (!row_legend.contains(rk))
                row_legend[rk] = e["row_title"];
            QJsonArray a;
            a.append(e["row"]);
            a.append(e["start"]);
            a.append(e["end"]);
            a.append(e.value("text"));
            if (e.contains("run")) a.append(e["run"]);
            tight.append(a);
        }
        r["schema"] = QJsonArray{"row", "start", "end", "text", "run?"};
        r["rows"]   = row_legend;
        r["events"] = tight;
    } else {
        r["events"] = events;
    }
    return r;
}

} // namespace mcp
} // namespace pv
