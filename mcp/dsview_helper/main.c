/*
 * dsview_helper — minimal CLI bridging libsigrok4DSL for the MCP server.
 *
 * Subcommands:
 *   list-devices                                   List attached devices (JSON)
 *   device-info [--index N]                        Active device info (JSON)
 *   capture  --output PFX [--index N]
 *            [--samplerate HZ] [--depth N]
 *            [--channels CSV]                       Capture logic samples
 *
 * Output of `capture`:
 *   <PFX>.bin   raw packed logic samples (unitsize bytes per slice)
 *   <PFX>.json  metadata + per-channel info
 *
 * All structured output is single-line JSON on stdout. Diagnostics go to stderr.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <time.h>
#include <inttypes.h>

#include <glib.h>
#include <libsigrok-internal.h>
#include <libsigrok.h>
#include <log/xlog.h>

#include "decode.h"

/* ---- shared state ----------------------------------------------------- */

static struct {
    FILE *bin;
    char *bin_path;
    char *json_path;

    uint64_t bytes_written;
    uint64_t samples_written;
    uint16_t unitsize;
    int      logic_format;        /* LA_CROSS_DATA / LA_SPLIT_DATA */
    bool     header_seen;

    volatile bool collect_done;
    volatile int  last_event;
    volatile bool error_flag;

    bool     verbose;
} g = {0};

static xlog_context *g_log_ctx = NULL;

/* ---- helpers ---------------------------------------------------------- */

static const char *json_escape(const char *src, char *buf, size_t buflen)
{
    size_t i = 0;
    if (!src) src = "";
    for (; *src && i + 2 < buflen; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            if (i + 3 >= buflen) break;
            buf[i++] = '\\';
            buf[i++] = (char)c;
        } else if (c < 0x20) {
            if (i + 7 >= buflen) break;
            i += snprintf(buf + i, buflen - i, "\\u%04x", c);
        } else {
            buf[i++] = (char)c;
        }
    }
    buf[i] = '\0';
    return buf;
}

static void emit_error(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char esc[1024];
    json_escape(msg, esc, sizeof(esc));
    printf("{\"ok\":false,\"error\":\"%s\"}\n", esc);
}

static void log_dbg(const char *fmt, ...)
{
    if (!g.verbose) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ---- callbacks -------------------------------------------------------- */

static void on_event(int event)
{
    g.last_event = event;
    log_dbg("[event] %d", event);
    if (event == DS_EV_COLLECT_TASK_END
        || event == DS_EV_DEVICE_STOPPED
        || event == DS_EV_COLLECT_TASK_END_BY_DETACHED
        || event == DS_EV_COLLECT_TASK_END_BY_ERROR) {
        if (event == DS_EV_COLLECT_TASK_END_BY_DETACHED
            || event == DS_EV_COLLECT_TASK_END_BY_ERROR)
            g.error_flag = true;
        g.collect_done = true;
    }
}

static void on_datafeed(const struct sr_dev_inst *sdi,
                        const struct sr_datafeed_packet *packet)
{
    (void)sdi;
    if (!packet) return;

    switch (packet->type) {
    case SR_DF_HEADER:
        g.header_seen = true;
        log_dbg("[df] HEADER");
        break;

    case SR_DF_LOGIC: {
        const struct sr_datafeed_logic *lo = packet->payload;
        if (!lo || !lo->data || lo->length == 0) break;

        if (g.unitsize == 0 && lo->unitsize > 0 && lo->unitsize <= 64) {
            g.unitsize     = lo->unitsize;
            g.logic_format = lo->format;
        }

        if (g.logic_format == LA_SPLIT_DATA) {
            /* Per-channel split. We don't write the file in this mode for v1
             * (would require interleaving on the fly). Just count for now. */
            g.bytes_written += lo->length;
            break;
        }

        /* LA_CROSS_DATA: each slice is `unitsize` bytes; channels packed. */
        if (g.bin) {
            size_t w = fwrite(lo->data, 1, lo->length, g.bin);
            if (w != lo->length) {
                fprintf(stderr, "fwrite short: %zu/%" PRIu64 "\n", w,
                        (uint64_t)lo->length);
                g.error_flag = true;
            }
        }
        g.bytes_written += lo->length;
        if (lo->unitsize)
            g.samples_written += lo->length / lo->unitsize;
        break;
    }

    case SR_DF_END:
        log_dbg("[df] END");
        g.collect_done = true;
        break;

    case SR_DF_OVERFLOW:
        fprintf(stderr, "warning: data overflow\n");
        break;

    default:
        break;
    }
}

/* ---- common init ------------------------------------------------------ */

static int ensure_lib_init(const char *firmware_dir, const char *user_data_dir)
{
    static bool inited = false;
    if (inited) return SR_OK;

    g_log_ctx = xlog_new();
    if (g_log_ctx) {
        ds_log_set_context(g_log_ctx);
        ds_log_level(g.verbose ? 5 : 1);
    }

    ds_set_event_callback(on_event);
    ds_set_datafeed_callback(on_datafeed);

    if (firmware_dir && *firmware_dir)
        ds_set_firmware_resource_dir(firmware_dir);
    if (user_data_dir && *user_data_dir)
        ds_set_user_data_dir(user_data_dir);

    int rc = ds_lib_init();
    if (rc != SR_OK) return rc;

    inited = true;
    return SR_OK;
}

/* ---- subcommand: list-devices ----------------------------------------- */

static int cmd_list_devices(const char *firmware_dir, const char *user_data_dir)
{
    int rc = ensure_lib_init(firmware_dir, user_data_dir);
    if (rc != SR_OK) {
        emit_error("ds_lib_init failed: %s", sr_error_str(rc));
        return 1;
    }

    /* Hotplug needs a brief moment to enumerate. */
    usleep(600 * 1000);
    ds_reload_device_list();

    struct ds_device_base_info *arr = NULL;
    int count = 0;
    rc = ds_get_device_list(&arr, &count);
    if (rc != SR_OK) {
        emit_error("ds_get_device_list failed: %s", sr_error_str(rc));
        return 1;
    }

    printf("{\"ok\":true,\"devices\":[");
    char esc[256];
    for (int i = 0; i < count; i++) {
        if (i) putchar(',');
        printf("{\"index\":%d,\"handle\":%llu,\"name\":\"%s\"}", i,
               (unsigned long long)arr[i].handle,
               json_escape(arr[i].name, esc, sizeof(esc)));
    }
    printf("]}\n");

    if (arr) g_free(arr);
    return 0;
}

/* ---- subcommand: device-info ------------------------------------------ */

static const char *operation_mode_name(int m)
{
    switch (m) {
    case LOGIC: return "LOGIC";
    case DSO: return "DSO";
    case ANALOG: return "ANALOG";
    default: return "UNKNOWN";
    }
}

static int activate_index(int index)
{
    ds_reload_device_list();
    int rc = ds_active_device_by_index(index);
    if (rc != SR_OK) return rc;
    return SR_OK;
}

static uint64_t get_u64_config(int key)
{
    GVariant *gv = NULL;
    uint64_t v = 0;
    if (ds_get_actived_device_config(NULL, NULL, key, &gv) == SR_OK && gv) {
        v = g_variant_get_uint64(gv);
        g_variant_unref(gv);
    }
    return v;
}

static int cmd_device_info(int index, const char *firmware_dir,
                           const char *user_data_dir)
{
    int rc = ensure_lib_init(firmware_dir, user_data_dir);
    if (rc != SR_OK) { emit_error("ds_lib_init failed"); return 1; }

    usleep(600 * 1000);
    if ((rc = activate_index(index)) != SR_OK) {
        emit_error("activate device %d failed: %s", index, sr_error_str(rc));
        return 1;
    }

    struct ds_device_full_info info; memset(&info, 0, sizeof(info));
    ds_get_actived_device_info(&info);

    int mode = ds_get_actived_device_mode();
    uint64_t rate = get_u64_config(SR_CONF_SAMPLERATE);
    uint64_t depth = get_u64_config(SR_CONF_LIMIT_SAMPLES);

    GSList *channels = ds_get_actived_device_channels();
    char esc[256];

    printf("{\"ok\":true,\"index\":%d,\"name\":\"%s\",\"driver\":\"%s\","
           "\"mode\":\"%s\",\"samplerate\":%" PRIu64 ",\"depth\":%" PRIu64
           ",\"channels\":[",
           index,
           json_escape(info.name, esc, sizeof(esc)),
           info.driver_name,
           operation_mode_name(mode),
           rate, depth);

    int first = 1;
    for (GSList *l = channels; l; l = l->next) {
        const struct sr_channel *ch = l->data;
        if (!ch) continue;
        if (!first) putchar(',');
        first = 0;
        printf("{\"index\":%u,\"name\":\"%s\",\"enabled\":%s,\"type\":%d}",
               ch->index,
               ch->name ? json_escape(ch->name, esc, sizeof(esc)) : "",
               ch->enabled ? "true" : "false",
               ch->type);
    }
    printf("]}\n");
    return 0;
}

/* ---- subcommand: capture ---------------------------------------------- */

static int set_u64_config(int key, uint64_t v)
{
    return ds_set_actived_device_config(NULL, NULL, key,
                                        g_variant_new_uint64(v));
}

static GSList *parse_csv_uint(const char *s)
{
    GSList *out = NULL;
    if (!s) return NULL;
    char *dup = g_strdup(s);
    for (char *tok = strtok(dup, ","); tok; tok = strtok(NULL, ",")) {
        while (*tok == ' ') tok++;
        long v = strtol(tok, NULL, 10);
        if (v >= 0)
            out = g_slist_append(out, GINT_TO_POINTER((int)v));
    }
    g_free(dup);
    return out;
}

struct capture_opts {
    /* -1 / negative / NULL means "leave at driver default". */
    int    operation_mode;     /* 0=Buffer, 1=Stream, ... */
    int    buffer_option;      /* 0=stop, 1=upload */
    int    filter;             /* 0=none, 1=1T */
    int    channel_mode;       /* driver-specific (16ch=0, 12ch=1, 6ch=2, 3ch=3 typ.) */
    int    rle;                /* 0/1 */
    int    ext_clock;          /* 0/1 (external sample clock) */
    int    falling_edge;       /* 0/1 (sample on falling clock edge) */
    const char *max_height;    /* "1X", "2X", etc. */
    /* Simple trigger (single-stage, single-probe). DSLogic only fires
     * triggers in Buffer mode; the helper auto-flips OPERATION_MODE if
     * a trigger is requested but the device is currently in Stream. */
    int    trigger_channel;    /* probe index, -1 = no trigger */
    char   trigger_edge;       /* 'R' rise | 'F' fall | 'C' either |
                                * '1' high | '0' low | 'X' off       */
    int    trigger_pos_pct;    /* 0..100 (pre-trigger %), -1 = default */
};

static int cmd_capture(int index, const char *output_prefix,
                       uint64_t samplerate, uint64_t depth,
                       const char *channels_csv,
                       int timeout_sec,
                       double vth_volts,
                       const struct capture_opts *opts,
                       const char *firmware_dir,
                       const char *user_data_dir)
{
    int rc = ensure_lib_init(firmware_dir, user_data_dir);
    if (rc != SR_OK) { emit_error("ds_lib_init failed"); return 1; }

    usleep(600 * 1000);

    /* Snapshot the device list *before* activation so we can verify the
     * activate request actually landed on the slot we asked for. USB
     * exclusivity races can leave libsigrok with a different active
     * device than expected (open fails internally, lib falls back).
     * Two same-name Plus units are common in the wild, so the post-
     * activation check must compare handle, not just name. */
    ds_device_handle expected_handle = NULL_HANDLE;
    char expected_name[150] = {0};
    {
        ds_reload_device_list();
        struct ds_device_base_info *arr = NULL;
        int count = 0;
        if (ds_get_device_list(&arr, &count) == SR_OK && arr && index >= 0
                && index < count) {
            expected_handle = arr[index].handle;
            strncpy(expected_name, arr[index].name,
                    sizeof(expected_name) - 1);
        }
        if (arr) g_free(arr);
    }

    if ((rc = activate_index(index)) != SR_OK) {
        emit_error("activate device %d failed: %s", index, sr_error_str(rc));
        return 1;
    }

    /* Verify what we actually got (kept in scope for metadata below). */
    static char actual_name[150];
    static ds_device_handle actual_handle;
    actual_name[0] = '\0';
    actual_handle = NULL_HANDLE;
    {
        struct ds_device_full_info dinfo; memset(&dinfo, 0, sizeof(dinfo));
        ds_get_actived_device_info(&dinfo);
        actual_handle = dinfo.handle;
        strncpy(actual_name, dinfo.name, sizeof(actual_name) - 1);
    }
    if (expected_handle != NULL_HANDLE && expected_handle != actual_handle) {
        emit_error("active-device mismatch: requested index=%d (handle "
                   "%llu, %s) but lib activated handle %llu (%s) — "
                   "likely USB exclusion (another process is holding "
                   "the device)",
                   index,
                   (unsigned long long)expected_handle, expected_name,
                   (unsigned long long)actual_handle, actual_name);
        return 1;
    }

    /* Configure channel selection (if requested). */
    GSList *want = parse_csv_uint(channels_csv);
    GSList *channels = ds_get_actived_device_channels();

    if (want) {
        /* First disable all logic channels */
        for (GSList *l = channels; l; l = l->next) {
            const struct sr_channel *ch = l->data;
            if (ch && ch->type == SR_CHANNEL_LOGIC)
                ds_enable_device_channel((struct sr_channel*)ch, FALSE);
        }
        /* Then enable selected */
        for (GSList *w = want; w; w = w->next) {
            int idx = GPOINTER_TO_INT(w->data);
            ds_enable_device_channel_index(idx, TRUE);
        }
        g_slist_free(want);
    }

    if (samplerate)
        if ((rc = set_u64_config(SR_CONF_SAMPLERATE, samplerate)) != SR_OK)
            fprintf(stderr, "warning: set samplerate %" PRIu64 " failed (%s)\n",
                    samplerate, sr_error_str(rc));

    if (depth)
        if ((rc = set_u64_config(SR_CONF_LIMIT_SAMPLES, depth)) != SR_OK)
            fprintf(stderr, "warning: set depth %" PRIu64 " failed (%s)\n",
                    depth, sr_error_str(rc));

    if (vth_volts > 0) {
        GVariant *gv = g_variant_new_double(vth_volts);
        rc = ds_set_actived_device_config(NULL, NULL, SR_CONF_VTH, gv);
        if (rc != SR_OK)
            fprintf(stderr, "warning: set VTH %.2fV failed (%s)\n",
                    vth_volts, sr_error_str(rc));
    }

    /* Optional GUI-equivalent device options. */
    if (opts) {
        if (opts->operation_mode >= 0) {
            GVariant *gv = g_variant_new_int16((int16_t)opts->operation_mode);
            rc = ds_set_actived_device_config(NULL, NULL,
                                              SR_CONF_OPERATION_MODE, gv);
            if (rc != SR_OK)
                fprintf(stderr, "warning: set operation_mode=%d failed (%s)\n",
                        opts->operation_mode, sr_error_str(rc));
        }
        if (opts->buffer_option >= 0) {
            GVariant *gv = g_variant_new_int16((int16_t)opts->buffer_option);
            rc = ds_set_actived_device_config(NULL, NULL,
                                              SR_CONF_BUFFER_OPTIONS, gv);
            if (rc != SR_OK)
                fprintf(stderr, "warning: set buffer_option=%d failed (%s)\n",
                        opts->buffer_option, sr_error_str(rc));
        }
        if (opts->filter >= 0) {
            GVariant *gv = g_variant_new_int16((int16_t)opts->filter);
            rc = ds_set_actived_device_config(NULL, NULL, SR_CONF_FILTER, gv);
            if (rc != SR_OK)
                fprintf(stderr, "warning: set filter=%d failed (%s)\n",
                        opts->filter, sr_error_str(rc));
        }
        if (opts->channel_mode >= 0) {
            GVariant *gv = g_variant_new_int16((int16_t)opts->channel_mode);
            rc = ds_set_actived_device_config(NULL, NULL,
                                              SR_CONF_CHANNEL_MODE, gv);
            if (rc != SR_OK)
                fprintf(stderr, "warning: set channel_mode=%d failed (%s)\n",
                        opts->channel_mode, sr_error_str(rc));
        }
        if (opts->rle >= 0) {
            GVariant *gv = g_variant_new_boolean(opts->rle ? TRUE : FALSE);
            rc = ds_set_actived_device_config(NULL, NULL, SR_CONF_RLE, gv);
            if (rc != SR_OK)
                fprintf(stderr, "warning: set rle=%d failed (%s)\n",
                        opts->rle, sr_error_str(rc));
        }
        if (opts->ext_clock >= 0) {
            GVariant *gv = g_variant_new_boolean(opts->ext_clock ? TRUE : FALSE);
            rc = ds_set_actived_device_config(NULL, NULL,
                                              SR_CONF_CLOCK_TYPE, gv);
            if (rc != SR_OK)
                fprintf(stderr, "warning: set ext_clock=%d failed (%s)\n",
                        opts->ext_clock, sr_error_str(rc));
        }
        if (opts->falling_edge >= 0) {
            GVariant *gv = g_variant_new_boolean(opts->falling_edge ? TRUE : FALSE);
            rc = ds_set_actived_device_config(NULL, NULL,
                                              SR_CONF_CLOCK_EDGE, gv);
            if (rc != SR_OK)
                fprintf(stderr, "warning: set falling_edge=%d failed (%s)\n",
                        opts->falling_edge, sr_error_str(rc));
        }
        if (opts->max_height && *opts->max_height) {
            GVariant *gv = g_variant_new_string(opts->max_height);
            rc = ds_set_actived_device_config(NULL, NULL,
                                              SR_CONF_MAX_HEIGHT, gv);
            if (rc != SR_OK)
                fprintf(stderr, "warning: set max_height=%s failed (%s)\n",
                        opts->max_height, sr_error_str(rc));
        }

        /* Simple trigger setup. DSLogic only honours triggers in Buffer
         * mode, so auto-flip OPERATION_MODE if the caller asked for a
         * trigger but didn't explicitly choose Buffer. Stream mode just
         * silently drops the trigger config otherwise. */
        if (opts->trigger_channel >= 0 && opts->trigger_edge != 'X') {
            if (opts->operation_mode < 0) {
                GVariant *gv = g_variant_new_int16(0); /* LO_OP_BUFFER */
                ds_set_actived_device_config(NULL, NULL,
                                             SR_CONF_OPERATION_MODE, gv);
                fprintf(stderr,
                        "info: trigger requested — auto-set Buffer mode\n");
            }
            ds_trigger_reset();
            ds_trigger_probe_set((uint16_t)opts->trigger_channel,
                                 (unsigned char)opts->trigger_edge,
                                 'X');
            ds_trigger_set_en(1);
            if (opts->trigger_pos_pct >= 0
                && opts->trigger_pos_pct <= 100) {
                ds_trigger_set_pos((uint16_t)opts->trigger_pos_pct);
            }
        }
    }

    /* Open output file */
    g.bin_path  = g_strdup_printf("%s.bin", output_prefix);
    g.json_path = g_strdup_printf("%s.json", output_prefix);
    g.bin = fopen(g.bin_path, "wb");
    if (!g.bin) {
        emit_error("open %s failed: %s", g.bin_path, strerror(errno));
        return 1;
    }

    g.bytes_written = 0;
    g.samples_written = 0;
    g.collect_done = false;
    g.error_flag = false;
    g.last_event = 0;
    g.unitsize = 0;
    g.logic_format = LA_CROSS_DATA;

    /* Pre-compute unitsize from currently-enabled logic channel count.
     * Some drivers (notably the demo) leave sr_datafeed_logic.unitsize
     * unset, so we trust the channel layout here. */
    int enabled_logic = 0;
    for (GSList *l = ds_get_actived_device_channels(); l; l = l->next) {
        const struct sr_channel *ch = l->data;
        if (ch && ch->type == SR_CHANNEL_LOGIC && ch->enabled)
            enabled_logic++;
    }
    uint16_t computed_unitsize = (uint16_t)((enabled_logic + 7) / 8);
    if (computed_unitsize == 0) computed_unitsize = 1;

    /* Detect device family — DSLogic / DSCope use atomic-block layout
     * (per-channel 8-byte chunks of 64 packed samples each, repeated).
     * Demo and others fall back to cross-byte layout. */
    struct ds_device_full_info dinfo; memset(&dinfo, 0, sizeof(dinfo));
    ds_get_actived_device_info(&dinfo);
    /* libsigrok4DSL emits LA_CROSS_DATA in a time-packed atomic-block layout
     * (each block = 64 samples × N enabled channels × 8 bytes/channel,
     * LSB-first packed) for every supported driver — DSLogic, DSCope, and
     * virtual-demo all use the same encoding. We default to atomic and only
     * fall back to legacy cross_byte if a future driver explicitly differs. */
    const char *layout = "dsl_atomic";
    (void)dinfo;

    rc = ds_start_collect();
    if (rc != SR_OK) {
        fclose(g.bin); g.bin = NULL;
        emit_error("ds_start_collect failed: %s", sr_error_str(rc));
        return 1;
    }

    time_t t0 = time(NULL);
    while (!g.collect_done) {
        usleep(50 * 1000);
        if (timeout_sec > 0 && (time(NULL) - t0) > timeout_sec) {
            fprintf(stderr, "capture timed out after %d s — stopping\n",
                    timeout_sec);
            ds_stop_collect();
            break;
        }
    }

    /* Drain any remaining packets briefly. */
    usleep(100 * 1000);

    fflush(g.bin);
    fclose(g.bin); g.bin = NULL;

    /* Re-read achieved settings (driver may have rounded). */
    uint64_t actual_rate = get_u64_config(SR_CONF_SAMPLERATE);
    uint64_t actual_depth = get_u64_config(SR_CONF_LIMIT_SAMPLES);
    uint64_t actual_samples = get_u64_config(SR_CONF_ACTUAL_SAMPLES);

    /* Trust packet-derived unitsize if it looks valid; otherwise fall back
     * to the channel-count derivation. */
    uint16_t final_unitsize = (g.unitsize > 0 && g.unitsize <= 8)
                                ? g.unitsize : computed_unitsize;
    /* For atomic-layout devices the driver packs `samples * en_ch` bits, so
     * sample count derives from total bytes and enabled-channel count, not
     * `length / unitsize` (which double-counts when en_ch < 8). */
    if (g_str_equal(layout, "dsl_atomic") && enabled_logic > 0)
        actual_samples = g.bytes_written * 8 / enabled_logic;
    else if (!actual_samples && final_unitsize > 0)
        actual_samples = g.bytes_written / final_unitsize;

    /* Write metadata. */
    FILE *jf = fopen(g.json_path, "w");
    if (!jf) {
        emit_error("open %s failed: %s", g.json_path, strerror(errno));
        return 1;
    }

    GSList *chs = ds_get_actived_device_channels();
    char esc[256];
    /* Build CSV of enabled-channel indices in driver order (which matches
     * the order channels are packed into atomic blocks). */
    GString *enabled_csv = g_string_new("");
    for (GSList *l = chs; l; l = l->next) {
        const struct sr_channel *ch = l->data;
        if (!ch || ch->type != SR_CHANNEL_LOGIC || !ch->enabled) continue;
        if (enabled_csv->len) g_string_append_c(enabled_csv, ',');
        g_string_append_printf(enabled_csv, "%u", ch->index);
    }

    fprintf(jf,
        "{\"ok\":true,"
        "\"bin\":\"%s\","
        "\"samplerate\":%" PRIu64 ","
        "\"requested_depth\":%" PRIu64 ","
        "\"actual_depth\":%" PRIu64 ","
        "\"samples_written\":%" PRIu64 ","
        "\"unitsize\":%u,"
        "\"format\":\"%s\","
        "\"layout\":\"%s\","
        "\"enabled_channels\":%d,"
        "\"enabled_indices\":\"%s\","
        "\"active_device_name\":\"%s\","
        "\"active_device_handle\":%llu,"
        "\"atomic_samples\":64,"
        "\"atomic_bytes_per_channel\":8,"
        "\"bytes\":%" PRIu64 ","
        "\"channels\":[",
        json_escape(g.bin_path, esc, sizeof(esc)),
        actual_rate, depth, actual_depth, actual_samples,
        final_unitsize,
        (g.logic_format == LA_SPLIT_DATA ? "split" : "cross"),
        layout, enabled_logic,
        enabled_csv->str,
        actual_name,
        (unsigned long long)actual_handle,
        g.bytes_written);
    g_string_free(enabled_csv, TRUE);
    int first = 1;
    for (GSList *l = chs; l; l = l->next) {
        const struct sr_channel *ch = l->data;
        if (!ch || ch->type != SR_CHANNEL_LOGIC) continue;
        if (!first) fputc(',', jf);
        first = 0;
        fprintf(jf, "{\"index\":%u,\"bit\":%u,\"name\":\"%s\",\"enabled\":%s}",
                ch->index, ch->index,
                ch->name ? json_escape(ch->name, esc, sizeof(esc)) : "",
                ch->enabled ? "true" : "false");
    }
    fprintf(jf, "],\"error\":%s}\n", g.error_flag ? "true" : "false");
    fclose(jf);

    /* Mirror metadata to stdout. */
    printf("{\"ok\":%s,\"bin\":\"%s\",\"json\":\"%s\","
           "\"samples\":%" PRIu64 ",\"samplerate\":%" PRIu64 ","
           "\"unitsize\":%u}\n",
           g.error_flag ? "false" : "true",
           g.bin_path, g.json_path,
           actual_samples, actual_rate, final_unitsize);

    g_free(g.bin_path); g.bin_path = NULL;
    g_free(g.json_path); g.json_path = NULL;
    return g.error_flag ? 2 : 0;
}

/* ---- subcommand: reset (release-and-rescan a stuck USB device) -------- */

static int cmd_reset_device(int index,
                            const char *firmware_dir,
                            const char *user_data_dir)
{
    int rc = ensure_lib_init(firmware_dir, user_data_dir);
    if (rc != SR_OK) {
        emit_error("reset: ds_lib_init failed: %s", sr_error_str(rc));
        return 1;
    }
    usleep(600 * 1000);
    ds_reload_device_list();

    /* Activate the requested slot, then release — this calls
     * close_device_instance() which closes the USB handle and tears
     * down the per-device session. Subsequent enumeration sees the
     * device fresh. */
    if ((rc = ds_active_device_by_index(index)) != SR_OK) {
        emit_error("reset: activate index %d failed: %s",
                   index, sr_error_str(rc));
        return 1;
    }

    char name[150] = {0};
    {
        struct ds_device_full_info dinfo;
        memset(&dinfo, 0, sizeof(dinfo));
        ds_get_actived_device_info(&dinfo);
        strncpy(name, dinfo.name, sizeof(name) - 1);
    }

    rc = ds_release_actived_device();
    if (rc != SR_OK) {
        emit_error("reset: ds_release_actived_device failed: %s",
                   sr_error_str(rc));
        return 1;
    }

    /* Let hotplug re-enumerate before returning so the next call
     * sees a fresh device list. */
    usleep(800 * 1000);
    ds_reload_device_list();

    char esc[300];
    printf("{\"ok\":true,\"reset\":true,\"index\":%d,\"name\":\"%s\"}\n",
           index, json_escape(name, esc, sizeof(esc)));
    fflush(stdout);
    return 0;
}

/* ---- subcommand: daemon (long-running, fixed-bind) -------------------- */

/* Tiny JSON-line scanners. The daemon protocol is one request per stdin
 * line, well-formed compact JSON; we only ever look up known keys. */

static char *json_get_str(const char *src, const char *key,
                          char *out, size_t outlen)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(src, pat);
    if (!p) { out[0] = '\0'; return NULL; }
    p += strlen(pat);
    const char *e = strchr(p, '"');
    if (!e) { out[0] = '\0'; return NULL; }
    size_t n = (size_t)(e - p);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return out;
}

static long json_get_int(const char *src, const char *key, long def_v)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(src, pat);
    if (!p) return def_v;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 'n' && strncmp(p, "null", 4) == 0) return def_v;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    return (end == p) ? def_v : v;
}

static double json_get_double(const char *src, const char *key, double def_v)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(src, pat);
    if (!p) return def_v;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 'n' && strncmp(p, "null", 4) == 0) return def_v;
    char *end = NULL;
    double v = strtod(p, &end);
    return (end == p) ? def_v : v;
}

static char parse_trigger_edge_word(const char *s)
{
    if (!s || !*s) return 'X';
    char c = s[0];
    if (c == 'R' || c == 'F' || c == 'C' || c == '1' || c == '0' ||
        c == 'r' || c == 'f' || c == 'c')
        return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if (!strcasecmp(s, "rising"))  return 'R';
    if (!strcasecmp(s, "falling")) return 'F';
    if (!strcasecmp(s, "either") || !strcasecmp(s, "edge")) return 'C';
    if (!strcasecmp(s, "high"))    return '1';
    if (!strcasecmp(s, "low"))     return '0';
    return 'X';
}

static int cmd_daemon(int bind_index,
                      const char *firmware_dir,
                      const char *user_data_dir)
{
    int rc = ensure_lib_init(firmware_dir, user_data_dir);
    if (rc != SR_OK) {
        emit_error("daemon: ds_lib_init failed: %s", sr_error_str(rc));
        return 1;
    }
    usleep(600 * 1000);

    /* Snapshot device list before activate so we can report what we
     * actually got bound to. */
    char bound_name[150] = {0};
    unsigned long long bound_handle = 0;
    {
        ds_reload_device_list();
        struct ds_device_base_info *arr = NULL;
        int count = 0;
        if (ds_get_device_list(&arr, &count) == SR_OK && arr
                && bind_index >= 0 && bind_index < count) {
            bound_handle = (unsigned long long)arr[bind_index].handle;
            strncpy(bound_name, arr[bind_index].name,
                    sizeof(bound_name) - 1);
        }
        if (arr) g_free(arr);
    }

    rc = ds_active_device_by_index(bind_index);
    if (rc != SR_OK) {
        emit_error("daemon: activate index %d failed: %s",
                   bind_index, sr_error_str(rc));
        return 1;
    }

    /* Verify by handle — same protection as cmd_capture has. */
    {
        struct ds_device_full_info dinfo;
        memset(&dinfo, 0, sizeof(dinfo));
        ds_get_actived_device_info(&dinfo);
        if (bound_handle != 0
                && bound_handle != (unsigned long long)dinfo.handle) {
            emit_error("daemon: bound index %d resolved to handle %llu "
                       "(%s) but lib activated handle %llu (%s)",
                       bind_index, bound_handle, bound_name,
                       (unsigned long long)dinfo.handle, dinfo.name);
            return 1;
        }
    }

    /* Tell the parent we're bound and ready. One JSON line. */
    char esc_name[300];
    printf("{\"ok\":true,\"daemon\":true,\"index\":%d,"
           "\"name\":\"%s\",\"handle\":%llu}\n",
           bind_index,
           json_escape(bound_name, esc_name, sizeof(esc_name)),
           bound_handle);
    fflush(stdout);

    /* Main request loop. One JSON request per line on stdin; one JSON
     * response line on stdout. The reused cmd_* helpers already print
     * single-line JSON results to stdout, so we mostly just dispatch
     * them and fflush. */
    char line[8192];
    while (fgets(line, sizeof(line), stdin)) {
        char method[64];
        json_get_str(line, "method", method, sizeof(method));

        if (!strcmp(method, "ping")) {
            printf("{\"ok\":true,\"pong\":true,\"index\":%d}\n",
                   bind_index);
            fflush(stdout);
            continue;
        }
        if (!strcmp(method, "shutdown")) {
            printf("{\"ok\":true,\"shutdown\":true}\n");
            fflush(stdout);
            return 0;
        }
        if (!strcmp(method, "device_info")) {
            cmd_device_info(bind_index, firmware_dir, user_data_dir);
            fflush(stdout);
            continue;
        }
        if (!strcmp(method, "reset")) {
            /* Release + re-activate the same slot. Lets the parent
             * unstick a hung USB session without killing the daemon. */
            int rc2 = ds_release_actived_device();
            usleep(800 * 1000);
            ds_reload_device_list();
            int rc3 = ds_active_device_by_index(bind_index);
            if (rc2 == SR_OK && rc3 == SR_OK) {
                printf("{\"ok\":true,\"reset\":true,\"index\":%d}\n",
                       bind_index);
            } else {
                printf("{\"ok\":false,\"error\":\"reset: release=%s, "
                       "reactivate=%s\"}\n",
                       sr_error_str(rc2), sr_error_str(rc3));
            }
            fflush(stdout);
            continue;
        }
        if (!strcmp(method, "capture")) {
            char output[512];
            json_get_str(line, "output", output, sizeof(output));
            if (!output[0]) {
                printf("{\"ok\":false,\"error\":\"capture: "
                       "output required\"}\n");
                fflush(stdout);
                continue;
            }
            uint64_t samplerate = (uint64_t)json_get_int(line,
                                                        "samplerate", 0);
            uint64_t depth      = (uint64_t)json_get_int(line, "depth", 0);
            char channels[256];
            json_get_str(line, "channels", channels, sizeof(channels));
            int timeout_sec = (int)json_get_int(line, "timeout", 30);
            double vth = json_get_double(line, "vth", 0.0);

            struct capture_opts opts = {
                .operation_mode  = (int)json_get_int(line,
                                                     "operation_mode", -1),
                .buffer_option   = (int)json_get_int(line,
                                                     "buffer_option", -1),
                .filter          = (int)json_get_int(line, "filter", -1),
                .channel_mode    = (int)json_get_int(line,
                                                     "channel_mode", -1),
                .rle             = (int)json_get_int(line, "rle", -1),
                .ext_clock       = (int)json_get_int(line, "ext_clock", -1),
                .falling_edge    = (int)json_get_int(line,
                                                     "falling_edge", -1),
                .max_height      = NULL,
                .trigger_channel = (int)json_get_int(line,
                                                     "trigger_channel", -1),
                .trigger_edge    = 'X',
                .trigger_pos_pct = (int)json_get_int(line,
                                                     "trigger_pos_pct", -1),
            };
            char max_height[32]; char trigger_edge_w[16];
            json_get_str(line, "max_height", max_height, sizeof(max_height));
            json_get_str(line, "trigger_edge",
                         trigger_edge_w, sizeof(trigger_edge_w));
            if (max_height[0]) opts.max_height = max_height;
            opts.trigger_edge = parse_trigger_edge_word(trigger_edge_w);

            cmd_capture(bind_index, output, samplerate, depth,
                        channels[0] ? channels : NULL,
                        timeout_sec, vth, &opts,
                        firmware_dir, user_data_dir);
            fflush(stdout);
            continue;
        }
        printf("{\"ok\":false,\"error\":\"unknown method '%s'\"}\n",
               method);
        fflush(stdout);
    }
    return 0;
}

/* ---- main ------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s <subcommand> [options]\n\n"
        "  list-devices  [--firmware DIR] [--user-data DIR]\n"
        "  device-info   [--index N] [--firmware DIR] [--user-data DIR]\n"
        "  capture       --output PREFIX [--index N]\n"
        "                [--samplerate HZ] [--depth N] [--channels CSV]\n"
        "                [--timeout SEC] [--firmware DIR] [--user-data DIR]\n"
        "  list-decoders [--decoders-dir DIR]\n"
        "  decode        --input PREFIX --protocol ID --map K=V[,...]\n"
        "                [--options K=V[,...]] [--decoders-dir DIR]\n"
        "                [--start N] [--end N] [--limit N]\n"
        "\nGlobal: --verbose for debug log to stderr.\n",
        prog);
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(argv[0]); return 64; }

    const char *cmd = argv[1];
    int    index = -1;
    const char *output = NULL;
    const char *channels = NULL;
    const char *firmware = NULL;
    const char *user_data = NULL;
    const char *protocol = NULL;
    const char *input    = NULL;
    const char *map      = NULL;
    const char *options  = NULL;
    const char *stack    = NULL;
    const char *decoders_dir = NULL;
    uint64_t samplerate = 0;
    uint64_t depth = 0;
    int timeout = 30;
    double vth_volts = 0.0;
    int bind_index = -1;
    struct capture_opts opts = {
        .operation_mode = -1,
        .buffer_option  = -1,
        .filter         = -1,
        .channel_mode   = -1,
        .rle            = -1,
        .ext_clock      = -1,
        .falling_edge   = -1,
        .max_height     = NULL,
        .trigger_channel = -1,
        .trigger_edge   = 'X',
        .trigger_pos_pct = -1,
    };
    long long start_sample = 0;
    long long end_sample = 0;
    int limit = 100000;

    static struct option longopts[] = {
        {"index",        required_argument, 0, 'i'},
        {"output",       required_argument, 0, 'o'},
        {"samplerate",   required_argument, 0, 'r'},
        {"depth",        required_argument, 0, 'd'},
        {"channels",     required_argument, 0, 'c'},
        {"timeout",      required_argument, 0, 't'},
        {"vth",          required_argument, 0, 'V'},
        {"operation-mode", required_argument, 0, 1001},
        {"buffer-option",  required_argument, 0, 1002},
        {"filter",         required_argument, 0, 1003},
        {"channel-mode",   required_argument, 0, 1004},
        {"rle",            required_argument, 0, 1005},
        {"ext-clock",      required_argument, 0, 1006},
        {"falling-edge",   required_argument, 0, 1007},
        {"max-height",     required_argument, 0, 1008},
        {"trigger-channel", required_argument, 0, 1009},
        {"trigger-edge",    required_argument, 0, 1010},
        {"trigger-pos",     required_argument, 0, 1011},
        {"bind-index",      required_argument, 0, 1012},
        {"firmware",     required_argument, 0, 'f'},
        {"user-data",    required_argument, 0, 'u'},
        {"protocol",     required_argument, 0, 'P'},
        {"input",        required_argument, 0, 'I'},
        {"map",          required_argument, 0, 'M'},
        {"options",      required_argument, 0, 'O'},
        {"decoders-dir", required_argument, 0, 'D'},
        {"start",        required_argument, 0, 'S'},
        {"end",          required_argument, 0, 'E'},
        {"limit",        required_argument, 0, 'L'},
        {"stack",        required_argument, 0, 1013},
        {"verbose",      no_argument,       0, 'v'},
        {"help",         no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    optind = 2;
    int c;
    while ((c = getopt_long(argc, argv,
                "i:o:r:d:c:t:V:f:u:P:I:M:O:D:S:E:L:vh",
                longopts, NULL)) != -1) {
        switch (c) {
        case 'i': index      = atoi(optarg); break;
        case 'o': output     = optarg;       break;
        case 'r': samplerate = strtoull(optarg, NULL, 10); break;
        case 'd': depth      = strtoull(optarg, NULL, 10); break;
        case 'c': channels   = optarg;       break;
        case 't': timeout    = atoi(optarg); break;
        case 'V': vth_volts  = atof(optarg); break;
        case 1001: opts.operation_mode = atoi(optarg); break;
        case 1002: opts.buffer_option  = atoi(optarg); break;
        case 1003: opts.filter         = atoi(optarg); break;
        case 1004: opts.channel_mode   = atoi(optarg); break;
        case 1005: opts.rle            = atoi(optarg); break;
        case 1006: opts.ext_clock      = atoi(optarg); break;
        case 1007: opts.falling_edge   = atoi(optarg); break;
        case 1008: opts.max_height     = optarg;       break;
        case 1009: opts.trigger_channel = atoi(optarg); break;
        case 1010: {
            char c = optarg[0];
            /* Accept either single-char encoding ('R'/'F'/'C'/'1'/'0')
             * or the user-friendly words used by the Python wrapper. */
            if (c == 'R' || c == 'F' || c == 'C' || c == '1' || c == '0' ||
                c == 'r' || c == 'f' || c == 'c') {
                opts.trigger_edge = (c >= 'a' && c <= 'z') ? c - 32 : c;
            } else if (!strcasecmp(optarg, "rising"))  opts.trigger_edge = 'R';
            else if  (!strcasecmp(optarg, "falling")) opts.trigger_edge = 'F';
            else if  (!strcasecmp(optarg, "either")
                  ||  !strcasecmp(optarg, "edge"))    opts.trigger_edge = 'C';
            else if  (!strcasecmp(optarg, "high"))    opts.trigger_edge = '1';
            else if  (!strcasecmp(optarg, "low"))     opts.trigger_edge = '0';
            break;
        }
        case 1011: opts.trigger_pos_pct = atoi(optarg); break;
        case 1012: bind_index = atoi(optarg); break;
        case 1013: stack       = optarg;       break;
        case 'f': firmware   = optarg;       break;
        case 'u': user_data  = optarg;       break;
        case 'P': protocol   = optarg;       break;
        case 'I': input      = optarg;       break;
        case 'M': map        = optarg;       break;
        case 'O': options    = optarg;       break;
        case 'D': decoders_dir = optarg;     break;
        case 'S': start_sample = strtoll(optarg, NULL, 10); break;
        case 'E': end_sample   = strtoll(optarg, NULL, 10); break;
        case 'L': limit        = atoi(optarg); break;
        case 'v': g.verbose  = true;         break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 64;
        }
    }

    if (index < 0) index = -1;   /* -1 = last */

    if (strcmp(cmd, "list-devices") == 0) {
        return cmd_list_devices(firmware, user_data);
    } else if (strcmp(cmd, "reset") == 0) {
        if (index < 0) { emit_error("reset: --index required"); return 64; }
        return cmd_reset_device(index, firmware, user_data);
    } else if (strcmp(cmd, "daemon") == 0) {
        int idx = bind_index >= 0 ? bind_index : index;
        if (idx < 0) { emit_error("daemon: --bind-index required"); return 64; }
        return cmd_daemon(idx, firmware, user_data);
    } else if (strcmp(cmd, "device-info") == 0) {
        return cmd_device_info(index, firmware, user_data);
    } else if (strcmp(cmd, "capture") == 0) {
        if (!output) { emit_error("capture: --output required"); return 64; }
        return cmd_capture(index, output, samplerate, depth, channels,
                           timeout, vth_volts, &opts, firmware, user_data);
    } else if (strcmp(cmd, "list-decoders") == 0) {
        return cmd_list_decoders(decoders_dir);
    } else if (strcmp(cmd, "decode") == 0) {
        if (!input || !protocol || !map) {
            emit_error("decode: --input, --protocol, --map are required");
            return 64;
        }
        return cmd_decode(input, protocol, map, options, stack,
                          decoders_dir, start_sample, end_sample, limit);
    } else {
        usage(argv[0]);
        return 64;
    }
}
