/*
 * dsview_helper — `decode` and `list-decoders` subcommands.
 *
 * Loads a previously-captured .bin/.json pair, transforms cross-packed logic
 * samples into per-channel bit-packed buffers (the format
 * libsigrokdecode4DSL expects), runs a decoder, and emits each annotation
 * as a JSON object on a single line under `annotations` array.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>

#include <glib.h>
#include <libsigrokdecode.h>

#include "decode.h"

/* ---- shared output state -------------------------------------------- */

static struct {
    int     count;
    int     limit;
    bool    truncated;
} ann_state;

/* ---- minimal JSON helpers ------------------------------------------ */

static void json_print_escaped(FILE *f, const char *s)
{
    fputc('"', f);
    if (!s) { fputc('"', f); return; }
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            fputc('\\', f); fputc((char)c, f);
        } else if (c == '\n') { fputs("\\n", f); }
          else if (c == '\r') { fputs("\\r", f); }
          else if (c == '\t') { fputs("\\t", f); }
          else if (c < 0x20)  { fprintf(f, "\\u%04x", c); }
          else                { fputc((char)c, f); }
    }
    fputc('"', f);
}

/* ---- annotation callback ------------------------------------------- */

static void on_annotation(struct srd_proto_data *pdata, void *cb_data)
{
    (void)cb_data;
    if (!pdata || !pdata->data || !pdata->pdo || !pdata->pdo->di) return;
    if (ann_state.count >= ann_state.limit) {
        ann_state.truncated = true;
        return;
    }

    const struct srd_proto_data_annotation *a = pdata->data;
    const struct srd_decoder *dec = pdata->pdo->di->decoder;

    /* The decoder->annotations GSList is built by prepend, so it is in
     * reverse order relative to a->ann_class. Each entry is a char** of
     * either {type_code, short, long} (3 elements) or {short, long} (2). */
    const char *class_label = "ann";
    int n_anns = g_slist_length(dec->annotations);
    GSList *l = g_slist_nth(dec->annotations, n_anns - 1 - a->ann_class);
    if (l && l->data) {
        char **pair = (char **)l->data;
        if (pair) {
            if (pair[0] && pair[1] && pair[2]) class_label = pair[1];
            else if (pair[0] && pair[1])       class_label = pair[0];
            else if (pair[0])                   class_label = pair[0];
        }
    }

    if (ann_state.count > 0) fputc(',', stdout);

    fputs("{\"start\":", stdout);
    fprintf(stdout, "%" PRIu64, pdata->start_sample);
    fputs(",\"end\":", stdout);
    fprintf(stdout, "%" PRIu64, pdata->end_sample);
    fputs(",\"class\":", stdout);
    fprintf(stdout, "%d", a->ann_class);
    fputs(",\"label\":", stdout);
    json_print_escaped(stdout, class_label);
    fputs(",\"text\":[", stdout);
    if (a->ann_text) {
        int first = 1;
        for (char **p = a->ann_text; *p; p++) {
            /* "\n" is the decoder's "ignore" sentinel for entries whose
             * actual numeric value is in str_number_hex; skip it. */
            if ((*p)[0] == '\n' && (*p)[1] == '\0') continue;
            if (!first) fputc(',', stdout);
            first = 0;
            json_print_escaped(stdout, *p);
        }
    }
    fputs("]", stdout);

    /* Hex string is sometimes filled in. */
    if (a->str_number_hex[0]) {
        fputs(",\"hex\":", stdout);
        json_print_escaped(stdout, a->str_number_hex);
    }
    fputc('}', stdout);

    ann_state.count++;
}

/* ---- bin/json loaders ---------------------------------------------- */

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = g_malloc(n + 1);
    if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); g_free(buf); return NULL; }
    buf[n] = '\0';
    if (out_len) *out_len = (size_t)n;
    fclose(f);
    return buf;
}

/* extremely small JSON-ish field extractor (just enough for our metadata) */
static int json_int_field(const char *json, const char *key, int64_t *out)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    char *end = NULL;
    long long v = strtoll(p, &end, 10);
    if (end == p) return -1;
    *out = v;
    return 0;
}

/* Pull a "key":"value" string field. Caller frees with g_free. */
static char *json_string_field(const char *json, const char *key)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    const char *end = strchr(p, '"');
    if (!end) return NULL;
    return g_strndup(p, end - p);
}

/* ---- map / option parsers ------------------------------------------ */

/* "scl=0,sda=1" -> GHashTable<char*, GVariant*int32> */
static GHashTable *parse_channel_map(const char *csv,
                                     int *out_max_logic_idx)
{
    *out_max_logic_idx = -1;
    GHashTable *h = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free,
                                          (GDestroyNotify)g_variant_unref);
    if (!csv || !*csv) return h;

    char *dup = g_strdup(csv);
    for (char *tok = strtok(dup, ","); tok; tok = strtok(NULL, ",")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = '\0';
        char *id = g_strstrip(tok);
        char *val = g_strstrip(eq + 1);
        int idx = atoi(val);
        GVariant *gv = g_variant_new_int32(idx);
        g_variant_ref_sink(gv);
        g_hash_table_insert(h, g_strdup(id), gv);
        if (idx > *out_max_logic_idx) *out_max_logic_idx = idx;
    }
    g_free(dup);
    return h;
}

/* "addr=8,bitorder=msb-first" -> GHashTable, with each value typed to
 * match the matching decoder option's default. */
static const struct srd_decoder_option *find_option(const struct srd_decoder *dec,
                                                    const char *id)
{
    for (GSList *l = dec ? dec->options : NULL; l; l = l->next) {
        const struct srd_decoder_option *o = l->data;
        if (o && o->id && strcmp(o->id, id) == 0) return o;
    }
    return NULL;
}

static GHashTable *parse_options(const char *csv,
                                 const struct srd_decoder *dec)
{
    GHashTable *h = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free,
                                          (GDestroyNotify)g_variant_unref);
    if (!csv || !*csv) return h;

    char *dup = g_strdup(csv);
    for (char *tok = strtok(dup, ","); tok; tok = strtok(NULL, ",")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = '\0';
        char *id = g_strstrip(tok);
        char *val = g_strstrip(eq + 1);

        const struct srd_decoder_option *opt = find_option(dec, id);
        GVariant *gv = NULL;

        if (opt && opt->def) {
            const GVariantType *t = g_variant_get_type(opt->def);
            if (g_variant_type_equal(t, G_VARIANT_TYPE_INT64)) {
                gv = g_variant_new_int64(strtoll(val, NULL, 10));
            } else if (g_variant_type_equal(t, G_VARIANT_TYPE_DOUBLE)) {
                gv = g_variant_new_double(strtod(val, NULL));
            } else {
                gv = g_variant_new_string(val);
            }
        } else {
            /* Unknown option — best-effort: parse as int if numeric. */
            char *endp = NULL;
            long long ll = strtoll(val, &endp, 10);
            if (endp != val && *endp == '\0')
                gv = g_variant_new_int64(ll);
            else
                gv = g_variant_new_string(val);
        }

        g_variant_ref_sink(gv);
        g_hash_table_insert(h, g_strdup(id), gv);
    }
    g_free(dup);
    return h;
}

/* ---- list-decoders -------------------------------------------------- */

int cmd_list_decoders(const char *decoders_dir)
{
    int rc = srd_init(decoders_dir);
    if (rc != SRD_OK) {
        fprintf(stdout, "{\"ok\":false,\"error\":\"srd_init failed: %s\"}\n",
                srd_strerror(rc));
        return 1;
    }
    rc = srd_decoder_load_all();
    if (rc != SRD_OK) {
        fprintf(stdout, "{\"ok\":false,\"error\":\"srd_decoder_load_all failed: %s\"}\n",
                srd_strerror(rc));
        srd_exit();
        return 1;
    }

    fputs("{\"ok\":true,\"decoders\":[", stdout);
    int first = 1;
    for (const GSList *l = srd_decoder_list(); l; l = l->next) {
        const struct srd_decoder *d = l->data;
        if (!d) continue;
        if (!first) fputc(',', stdout);
        first = 0;

        fputs("{\"id\":", stdout);    json_print_escaped(stdout, d->id);
        fputs(",\"name\":", stdout);  json_print_escaped(stdout, d->name);
        fputs(",\"longname\":", stdout); json_print_escaped(stdout, d->longname);
        fputs(",\"desc\":", stdout);  json_print_escaped(stdout, d->desc);

        fputs(",\"channels\":[", stdout);
        int cf = 1;
        for (GSList *ch = d->channels; ch; ch = ch->next) {
            const struct srd_channel *c = ch->data;
            if (!c) continue;
            if (!cf) fputc(',', stdout);
            cf = 0;
            fputc('{', stdout);
            fputs("\"id\":", stdout); json_print_escaped(stdout, c->id);
            fputs(",\"name\":", stdout); json_print_escaped(stdout, c->name);
            fputs(",\"required\":true", stdout);
            fputc('}', stdout);
        }
        for (GSList *ch = d->opt_channels; ch; ch = ch->next) {
            const struct srd_channel *c = ch->data;
            if (!c) continue;
            if (!cf) fputc(',', stdout);
            cf = 0;
            fputc('{', stdout);
            fputs("\"id\":", stdout); json_print_escaped(stdout, c->id);
            fputs(",\"name\":", stdout); json_print_escaped(stdout, c->name);
            fputs(",\"required\":false", stdout);
            fputc('}', stdout);
        }
        fputc(']', stdout);

        fputs(",\"options\":[", stdout);
        int of = 1;
        for (GSList *op = d->options; op; op = op->next) {
            const struct srd_decoder_option *o = op->data;
            if (!o) continue;
            if (!of) fputc(',', stdout);
            of = 0;
            fputc('{', stdout);
            fputs("\"id\":", stdout); json_print_escaped(stdout, o->id);
            fputs(",\"desc\":", stdout); json_print_escaped(stdout, o->desc ? o->desc : "");
            fputc('}', stdout);
        }
        fputc(']', stdout);

        fputc('}', stdout);
    }
    fputs("]}\n", stdout);

    srd_exit();
    return 0;
}

/* ---- decode --------------------------------------------------------- */

int cmd_decode(const char *input_prefix,
               const char *protocol,
               const char *map_csv,
               const char *options_csv,
               const char *stack_csv,
               const char *decoders_dir,
               long long start_sample,
               long long end_sample,
               int max_annotations)
{
    int rc;

    /* --- read metadata --- */
    char *json_path = g_strdup_printf("%s.json", input_prefix);
    char *bin_path  = g_strdup_printf("%s.bin",  input_prefix);

    size_t json_len = 0;
    char *json = read_file(json_path, &json_len);
    if (!json) {
        fprintf(stdout, "{\"ok\":false,\"error\":\"cannot read %s: %s\"}\n",
                json_path, strerror(errno));
        g_free(json_path); g_free(bin_path);
        return 1;
    }

    int64_t samplerate = 0, unitsize = 0, samples_written = 0;
    int64_t atom_samples = 0, atom_bytes = 0;
    json_int_field(json, "samplerate", &samplerate);
    json_int_field(json, "unitsize",   &unitsize);
    json_int_field(json, "samples_written", &samples_written);
    json_int_field(json, "atomic_samples", &atom_samples);
    json_int_field(json, "atomic_bytes_per_channel", &atom_bytes);
    char *layout = json_string_field(json, "layout");
    char *enabled_csv = json_string_field(json, "enabled_indices");
    g_free(json);

    if (unitsize <= 0 || samplerate <= 0) {
        fprintf(stdout, "{\"ok\":false,\"error\":\"bad metadata: samplerate=%" PRId64
                " unitsize=%" PRId64 "\"}\n", samplerate, unitsize);
        g_free(json_path); g_free(bin_path);
        return 1;
    }

    /* --- read bin --- */
    size_t bin_len = 0;
    uint8_t *bin = (uint8_t *)read_file(bin_path, &bin_len);
    if (!bin) {
        fprintf(stdout, "{\"ok\":false,\"error\":\"cannot read %s: %s\"}\n",
                bin_path, strerror(errno));
        g_free(layout); g_free(enabled_csv);
        g_free(json_path); g_free(bin_path);
        return 1;
    }

    /* DSLogic / DSCope / virtual-demo emit a time-packed atomic-block layout:
     * each block holds atom_samples samples × N enabled channels, stored as
     * N consecutive (atom_bytes) per-channel runs of LSB-first packed bits.
     * Decoders downstream want the legacy cross-byte format
     * (one `unitsize`-byte slice per sample, channel C in bit (C%8) of
     * byte (C/8)), so transform once here. */
    if (layout && g_str_equal(layout, "dsl_atomic")
            && atom_samples > 0 && atom_bytes > 0
            && enabled_csv && *enabled_csv) {
        /* Parse enabled-channel index list. */
        GArray *en = g_array_new(FALSE, FALSE, sizeof(int));
        char *dup = g_strdup(enabled_csv);
        for (char *t = strtok(dup, ","); t; t = strtok(NULL, ",")) {
            int v = atoi(t);
            g_array_append_val(en, v);
        }
        g_free(dup);
        int en_count = (int)en->len;
        size_t block_bytes = (size_t)atom_bytes * (size_t)en_count;
        /* The cross slice must be wide enough to hold every enabled channel
         * at its raw index — e.g. enabling only ch 12-15 still needs
         * unitsize=2 because we drop the bit at byte (ch/8). The capture's
         * recorded unitsize is computed from enabled_count, which is too
         * small for sparse-high enable sets, so re-derive it from max ch. */
        int max_ch = -1;
        for (int i = 0; i < en_count; i++) {
            int v = g_array_index(en, int, i);
            if (v > max_ch) max_ch = v;
        }
        int needed_unitsize = (max_ch >= 0) ? (max_ch / 8 + 1) : 1;
        if (needed_unitsize > unitsize)
            unitsize = needed_unitsize;

        if (en_count > 0 && block_bytes > 0) {
            size_t n_blocks = bin_len / block_bytes;
            size_t cross_bytes = n_blocks * (size_t)atom_samples
                                  * (size_t)unitsize;
            uint8_t *cross = g_malloc0(cross_bytes);
            for (size_t blk = 0; blk < n_blocks; blk++) {
                const uint8_t *blk_base = bin + blk * block_bytes;
                for (int slot = 0; slot < en_count; slot++) {
                    int ch = g_array_index(en, int, slot);
                    size_t byte_in_slice = (size_t)ch / 8;
                    uint8_t mask = (uint8_t)(1u << (ch % 8));
                    const uint8_t *src = blk_base + (size_t)slot * atom_bytes;
                    for (int s = 0; s < atom_samples; s++) {
                        if (src[s / 8] & (1u << (s % 8))) {
                            size_t global = blk * (size_t)atom_samples + s;
                            cross[global * (size_t)unitsize + byte_in_slice]
                                |= mask;
                        }
                    }
                }
            }
            g_free(bin);
            bin = cross;
            bin_len = cross_bytes;
        }
        g_array_free(en, TRUE);
    }
    g_free(layout); layout = NULL;
    g_free(enabled_csv); enabled_csv = NULL;

    int64_t total_samples = (int64_t)(bin_len / (size_t)unitsize);

    if (start_sample < 0) start_sample = 0;
    if (end_sample <= 0 || end_sample > total_samples)
        end_sample = total_samples;
    if (end_sample <= start_sample) {
        fprintf(stdout, "{\"ok\":false,\"error\":\"empty sample range\"}\n");
        g_free(json_path); g_free(bin_path); g_free(bin);
        return 1;
    }
    int64_t window = end_sample - start_sample;

    /* --- init libsigrokdecode --- */
    rc = srd_init(decoders_dir);
    if (rc != SRD_OK) {
        fprintf(stdout, "{\"ok\":false,\"error\":\"srd_init failed: %s\"}\n",
                srd_strerror(rc));
        g_free(json_path); g_free(bin_path); g_free(bin);
        return 1;
    }

    /* Decoders' Python module name uses '-' where the in-system id uses ':'
     * (DSL fork convention: id "0:uart" lives in module "0-uart"). Try the
     * caller's spelling first, then the ':' -> '-' translation. */
    rc = srd_decoder_load(protocol);
    if (rc != SRD_OK) {
        char *alt = g_strdup(protocol);
        for (char *p = alt; *p; p++) if (*p == ':') *p = '-';
        rc = srd_decoder_load(alt);
        g_free(alt);
    }
    if (rc != SRD_OK) {
        fprintf(stdout, "{\"ok\":false,\"error\":\"failed to load decoder %s: %s\"}\n",
                protocol, srd_strerror(rc));
        srd_exit();
        g_free(json_path); g_free(bin_path); g_free(bin);
        return 1;
    }

    const struct srd_decoder *dec = srd_decoder_get_by_id(protocol);
    if (!dec) {
        fprintf(stdout, "{\"ok\":false,\"error\":\"decoder %s not found after load\"}\n",
                protocol);
        srd_exit();
        g_free(json_path); g_free(bin_path); g_free(bin);
        return 1;
    }

    int num_required = g_slist_length(dec->channels);
    int num_optional = g_slist_length(dec->opt_channels);
    int num_total    = num_required + num_optional;

    /* --- parse maps --- */
    int max_logic_idx = -1;
    GHashTable *channel_map = parse_channel_map(map_csv, &max_logic_idx);
    GHashTable *opt_map     = parse_options(options_csv, dec);

    if ((size_t)(max_logic_idx / 8) >= (size_t)unitsize) {
        fprintf(stdout, "{\"ok\":false,\"error\":\"logic channel %d out of range "
                "(unitsize=%" PRId64 ")\"}\n", max_logic_idx, unitsize);
        srd_exit();
        g_hash_table_destroy(channel_map);
        g_hash_table_destroy(opt_map);
        g_free(json_path); g_free(bin_path); g_free(bin);
        return 1;
    }

    /* --- session --- */
    struct srd_session *sess = NULL;
    if (srd_session_new(&sess) != SRD_OK || !sess) {
        fprintf(stdout, "{\"ok\":false,\"error\":\"srd_session_new failed\"}\n");
        srd_exit();
        g_free(json_path); g_free(bin_path); g_free(bin);
        return 1;
    }

    struct srd_decoder_inst *di = srd_inst_new(sess, protocol, opt_map);
    g_hash_table_destroy(opt_map);
    if (!di) {
        fprintf(stdout, "{\"ok\":false,\"error\":\"srd_inst_new failed for %s\"}\n",
                protocol);
        srd_session_destroy(sess); srd_exit();
        g_hash_table_destroy(channel_map);
        g_free(json_path); g_free(bin_path); g_free(bin);
        return 1;
    }

    if (srd_inst_channel_set_all(di, channel_map) != SRD_OK) {
        fprintf(stdout, "{\"ok\":false,\"error\":\"srd_inst_channel_set_all failed\"}\n");
        srd_session_destroy(sess); srd_exit();
        g_hash_table_destroy(channel_map);
        g_free(json_path); g_free(bin_path); g_free(bin);
        return 1;
    }
    g_hash_table_destroy(channel_map);

    if (srd_session_metadata_set(sess, SRD_CONF_SAMPLERATE,
                g_variant_new_uint64((uint64_t)samplerate)) != SRD_OK) {
        fprintf(stdout, "{\"ok\":false,\"error\":\"srd_session_metadata_set failed\"}\n");
        srd_session_destroy(sess); srd_exit();
        g_free(json_path); g_free(bin_path); g_free(bin);
        return 1;
    }

    /* --- stacked decoders ---
     * `stack_csv` is a comma-separated list of upper-layer protocol ids
     * to stack on top of the base inst. The DSL fork prefixes the same
     * protocol id with `0:` / `1:` for two variants — pass through
     * verbatim, falling back to `:` -> `-` rewrite for the Python
     * module name. Per-stack options are not exposed yet; most common
     * stacks (24xx EEPROM, spiflash, mdio) work with defaults. */
    if (stack_csv && *stack_csv) {
        char *dup = g_strdup(stack_csv);
        struct srd_decoder_inst *prev = di;
        for (char *tok = strtok(dup, ","); tok; tok = strtok(NULL, ",")) {
            char *id = g_strstrip(tok);
            if (!*id) continue;

            int rc2 = srd_decoder_load(id);
            if (rc2 != SRD_OK) {
                char *alt = g_strdup(id);
                for (char *p = alt; *p; p++) if (*p == ':') *p = '-';
                rc2 = srd_decoder_load(alt);
                g_free(alt);
            }
            if (rc2 != SRD_OK) {
                fprintf(stderr, "warning: stacked decoder '%s' load failed: %s\n",
                        id, srd_strerror(rc2));
                continue;
            }
            GHashTable *uopts = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                      g_free,
                                                      (GDestroyNotify)g_variant_unref);
            struct srd_decoder_inst *uinst = srd_inst_new(sess, id, uopts);
            g_hash_table_destroy(uopts);
            if (!uinst) {
                fprintf(stderr, "warning: srd_inst_new failed for stacked '%s'\n",
                        id);
                continue;
            }
            if (srd_inst_stack(sess, prev, uinst) != SRD_OK) {
                fprintf(stderr, "warning: srd_inst_stack failed for '%s'\n", id);
                continue;
            }
            prev = uinst;
        }
        g_free(dup);
    }

    ann_state.count = 0;
    ann_state.limit = max_annotations > 0 ? max_annotations : 100000;
    ann_state.truncated = false;
    srd_pd_output_callback_add(sess, SRD_OUTPUT_ANN, on_annotation, NULL);

    char *err = NULL;
    if (srd_session_start(sess, &err) != SRD_OK) {
        fprintf(stdout, "{\"ok\":false,\"error\":\"srd_session_start: %s\"}\n",
                err ? err : "?");
        if (err) g_free(err);
        srd_session_destroy(sess); srd_exit();
        g_free(json_path); g_free(bin_path); g_free(bin);
        return 1;
    }

    /* --- build per-decoder-channel bit-packed buffers ---
     * For each decoder channel j, dec_channelmap[j] now holds the
     * input-data channel index (which we set to the logic channel index
     * via channel_map). We must produce inbuf[j] -> packed buffer for
     * that logic channel over [start_sample, end_sample). */
    int *dec_chmap = di->dec_channelmap;
    size_t packed_bytes = (size_t)((window + 7) / 8);

    uint8_t **inbuf = g_malloc0(sizeof(uint8_t *) * num_total);
    uint8_t  *inbuf_const = g_malloc0(num_total);
    uint8_t **packed = g_malloc0(sizeof(uint8_t *) * num_total);

    for (int j = 0; j < num_total; j++) {
        int logic_ch = dec_chmap[j];
        if (logic_ch < 0) {
            inbuf[j] = NULL;
            inbuf_const[j] = 0;
            continue;
        }
        size_t byte_in_slice = (size_t)logic_ch / 8;
        uint8_t mask = (uint8_t)(1u << (logic_ch % 8));

        uint8_t *p = g_malloc0(packed_bytes);
        packed[j] = p;

        for (int64_t n = 0; n < window; n++) {
            int64_t src = (start_sample + n) * unitsize + (int64_t)byte_in_slice;
            uint8_t bit = (bin[src] & mask) ? 1 : 0;
            if (bit) p[n / 8] |= (uint8_t)(1u << (n % 8));
        }
        inbuf[j] = p;
    }

    /* --- emit JSON header & stream annotations --- */
    fputs("{\"ok\":true,\"protocol\":", stdout);
    json_print_escaped(stdout, protocol);
    fprintf(stdout,
            ",\"start_sample\":%" PRId64 ",\"end_sample\":%" PRId64
            ",\"samplerate\":%" PRId64 ",\"annotations\":[",
            start_sample, end_sample, samplerate);
    fflush(stdout);

    /* Send all samples in one chunk. (The first arg `abs_start` is 0 in
     * the decoder's view; samplenum offsets in callback are relative to
     * that.) */
    err = NULL;
    rc = srd_session_send(sess, 0, (uint64_t)window,
                          (const uint8_t **)inbuf, inbuf_const,
                          (uint64_t)window, &err);
    if (rc != SRD_OK) {
        fprintf(stderr, "srd_session_send failed: %s\n", err ? err : "?");
        if (err) { g_free(err); err = NULL; }
    }

    err = NULL;
    srd_session_end(sess, &err);
    if (err) { g_free(err); }

    fprintf(stdout, "],\"count\":%d,\"truncated\":%s,\"window_samples\":%" PRId64 "}\n",
            ann_state.count, ann_state.truncated ? "true" : "false", window);

    /* --- cleanup --- */
    for (int j = 0; j < num_total; j++) g_free(packed[j]);
    g_free(packed);
    g_free(inbuf);
    g_free(inbuf_const);
    srd_session_destroy(sess);
    srd_exit();
    g_free(json_path); g_free(bin_path); g_free(bin);

    return 0;
}
