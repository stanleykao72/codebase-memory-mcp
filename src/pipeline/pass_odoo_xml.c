/*
 * pass_odoo_xml.c — Odoo fork (Tier C), main-sequence pass.
 *
 * Odoo view definitions live in XML records the generic XML extractor cannot
 * interpret. This pass scans .xml files for `<record model="ir.ui.view">`
 * elements and builds the view layer of the graph:
 *
 *   - View node per ir.ui.view record, QN = "__view__<xml_id>";
 *   - FOR_MODEL: View -> Model node named by the record's <field name="model">
 *     (reuses the Tier B Model nodes; creates the node if the model is only
 *     referenced, e.g. a core model);
 *   - EXTENDS: View -> parent View for <field name="inherit_id" ref="...">,
 *     capturing Odoo's view-inheritance (inherit_id / xpath) layer.
 *
 * Odoo XML is highly regular, so a targeted text scan (not a full AST walk) is
 * used: robust for standard records, and free of tree-sitter XML navigation.
 * Runs after definitions (Model nodes exist) in both the sequential and
 * parallel pipelines.
 */
#include "pipeline/pipeline_internal.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/log.h"
#include "cbm.h" /* CBMLanguage, cbm_file_info_t via pipeline.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ODOO_VIEW_QN_PREFIX "__view__"
#define ODOO_VIEW_QN_SIZE 320

static bool memmem_n(const char *hay, size_t n, const char *needle);

static const char *itoa_xml(int v) {
    static _Thread_local char bufs[4][32];
    static _Thread_local int idx = 0;
    int i = idx;
    idx = (idx + 1) & 3;
    snprintf(bufs[i], sizeof(bufs[i]), "%d", v);
    return bufs[i];
}

/* Read an entire file into a NUL-terminated heap buffer (or NULL). */
static char *xml_read_file(const char *path, long *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (64L * 1024 * 1024)) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len) {
        *out_len = (long)rd;
    }
    return buf;
}

/* Copy the value of attribute `name` found within [tag, tag_end) into out.
 * Matches `name="..."` or `name='...'`. Returns true on success. */
static bool attr_value(const char *tag, const char *tag_end, const char *name, char *out,
                       size_t outsz) {
    size_t nlen = strlen(name);
    for (const char *p = tag; p && p + nlen + 2 < tag_end; p++) {
        if (strncmp(p, name, nlen) != 0) {
            continue;
        }
        /* attribute boundary: preceding char is space/quote/<, next is = or space */
        if (p != tag && !(p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == '<')) {
            continue;
        }
        const char *q = p + nlen;
        while (q < tag_end && (*q == ' ' || *q == '\t' || *q == '\n')) {
            q++;
        }
        if (q >= tag_end || *q != '=') {
            continue;
        }
        q++;
        while (q < tag_end && (*q == ' ' || *q == '\t' || *q == '\n')) {
            q++;
        }
        if (q >= tag_end || (*q != '"' && *q != '\'')) {
            continue;
        }
        char quote = *q++;
        const char *start = q;
        while (q < tag_end && *q != quote) {
            q++;
        }
        if (q >= tag_end) {
            return false;
        }
        size_t len = (size_t)(q - start);
        if (len >= outsz) {
            len = outsz - 1;
        }
        memcpy(out, start, len);
        out[len] = '\0';
        return true;
    }
    return false;
}

/* Within [body, body_end), find `<field name="model">VALUE</field>` and copy
 * VALUE into out. Returns true on success. */
static bool field_text(const char *body, const char *body_end, const char *fname, char *out,
                       size_t outsz) {
    char needle[64];
    snprintf(needle, sizeof(needle), "name=\"%s\"", fname);
    for (const char *p = body; (p = strstr(p, "<field")) != NULL && p < body_end; p += 6) {
        const char *gt = strchr(p, '>');
        if (!gt || gt >= body_end) {
            return false;
        }
        /* this field's open tag is [p, gt] */
        if (!memmem_n(p, (size_t)(gt - p), needle)) {
            continue;
        }
        const char *val = gt + 1;
        const char *end = strstr(val, "</field>");
        if (!end || end > body_end) {
            return false;
        }
        size_t len = (size_t)(end - val);
        while (len > 0 && (val[0] == ' ' || val[0] == '\n' || val[0] == '\t')) {
            val++;
            len--;
        }
        while (len > 0 && (val[len - 1] == ' ' || val[len - 1] == '\n' || val[len - 1] == '\t')) {
            len--;
        }
        if (len == 0 || len >= outsz) {
            return false;
        }
        memcpy(out, val, len);
        out[len] = '\0';
        return true;
    }
    return false;
}

/* Substring search bounded to n bytes (no NUL assumption on haystack range). */
static bool memmem_n(const char *hay, size_t n, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0 || nl > n) {
        return false;
    }
    for (size_t i = 0; i + nl <= n; i++) {
        if (memcmp(hay + i, needle, nl) == 0) {
            return true;
        }
    }
    return false;
}

/* Within [body, body_end), find the inherit_id field and copy its ref= value. */
static bool inherit_ref(const char *body, const char *body_end, char *out, size_t outsz) {
    for (const char *p = body; (p = strstr(p, "<field")) != NULL && p < body_end; p += 6) {
        const char *gt = strchr(p, '>');
        if (!gt || gt >= body_end) {
            return false;
        }
        if (memmem_n(p, (size_t)(gt - p), "name=\"inherit_id\"")) {
            return attr_value(p, gt, "ref", out, outsz);
        }
    }
    return false;
}

/* Upsert a View node by xml id, returning its temp id. */
static int64_t ensure_view_node(cbm_gbuf_t *gb, const char *xml_id, const char *rel) {
    if (!xml_id || !xml_id[0]) {
        return 0;
    }
    char qn[ODOO_VIEW_QN_SIZE];
    snprintf(qn, sizeof(qn), ODOO_VIEW_QN_PREFIX "%s", xml_id);
    const cbm_gbuf_node_t *existing = cbm_gbuf_find_by_qn(gb, qn);
    if (existing) {
        return existing->id;
    }
    return cbm_gbuf_upsert_node(gb, "View", xml_id, qn, rel ? rel : "", 0, 0, "{}");
}

int cbm_pipeline_pass_odoo_xml(cbm_pipeline_ctx_t *ctx, const cbm_file_info_t *files,
                               int file_count) {
    if (!ctx || !ctx->gbuf || !files) {
        return 0;
    }
    int views = 0, for_model = 0, extends = 0;

    for (int fi = 0; fi < file_count; fi++) {
        if (files[fi].language != CBM_LANG_XML) {
            continue;
        }
        long len = 0;
        char *src = xml_read_file(files[fi].path, &len);
        if (!src) {
            continue;
        }
        const char *rel = files[fi].rel_path;
        const char *end = src + len;

        for (const char *p = src; (p = strstr(p, "<record")) != NULL;) {
            const char *tag_end = strchr(p, '>');
            if (!tag_end) {
                break;
            }
            char model_attr[128] = {0};
            char xml_id[192] = {0};
            attr_value(p, tag_end, "model", model_attr, sizeof(model_attr));
            attr_value(p, tag_end, "id", xml_id, sizeof(xml_id));

            const char *rec_end = strstr(tag_end, "</record>");
            if (!rec_end) {
                rec_end = end;
            }

            if (strcmp(model_attr, "ir.ui.view") == 0 && xml_id[0]) {
                int64_t view_id = ensure_view_node(ctx->gbuf, xml_id, rel);
                if (view_id > 0) {
                    views++;
                    /* FOR_MODEL: <field name="model">m.n</field> */
                    char model_val[128];
                    if (field_text(tag_end, rec_end, "model", model_val, sizeof(model_val))) {
                        int64_t m = cbm_odoo_ensure_model_node(ctx->gbuf, model_val);
                        if (m > 0 &&
                            cbm_gbuf_insert_edge(ctx->gbuf, view_id, m, "FOR_MODEL", "{}")) {
                            for_model++;
                        }
                    }
                    /* EXTENDS: <field name="inherit_id" ref="parent"/> */
                    char ref[192];
                    if (inherit_ref(tag_end, rec_end, ref, sizeof(ref))) {
                        int64_t parent = ensure_view_node(ctx->gbuf, ref, NULL);
                        if (parent > 0 &&
                            cbm_gbuf_insert_edge(ctx->gbuf, view_id, parent, "EXTENDS", "{}")) {
                            extends++;
                        }
                    }
                }
            }
            p = (rec_end < end) ? rec_end + 1 : end;
            if (p >= end) {
                break;
            }
        }
        free(src);
    }

    cbm_log_info("pass.odoo_xml", "views", itoa_xml(views), "for_model", itoa_xml(for_model),
                 "extends", itoa_xml(extends));
    return 0;
}
