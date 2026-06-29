/*
 * pass_odoo_model.c — Odoo fork (Tier B), predump pass.
 *
 * Builds the browsable Odoo model graph from the odoo_model_name / odoo_inherit_list
 * properties that the Python class extractor recorded on Class nodes:
 *
 *   - one synthetic "Model" node per Odoo model name, QN = "__model__<name>"
 *     (dedup across the many classes that contribute to one model);
 *   - DEFINES_MODEL: Class node -> the Model it declares (_name) or extends
 *     (_inherit, for a pure-extension class with no own _name);
 *   - INHERITS_MODEL: Model(_name) -> Model(parent) for each _inherit / _inherits
 *     target, so the inheritance chain is traversable.
 *
 * Runs after definitions are in the graph buffer (the Class nodes carry the
 * serialized odoo_* properties) and is single-path: it reads gbuf node
 * properties, so sequential and parallel indexing produce identical results.
 * ORM call edges (self.env['x'].method) are emitted separately during call
 * resolution (pass_calls.c / pass_parallel.c).
 */
#include "pipeline/pipeline_internal.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/log.h"
#include <yyjson/yyjson.h>
#include <stdio.h>
#include <string.h>

#define ODOO_MODEL_QN_PREFIX "__model__"
#define ODOO_MODEL_QN_SIZE 320

/* Thread-local rotating buffers for small int→string conversions (logging). */
static const char *itoa_buf(int v) {
    static _Thread_local char bufs[4][32];
    static _Thread_local int idx = 0;
    int i = idx;
    idx = (idx + 1) & 3;
    snprintf(bufs[i], sizeof(bufs[i]), "%d", v);
    return bufs[i];
}

/* Build the deterministic Model node QN for a model name. */
void cbm_odoo_model_qn(const char *name, char *buf, size_t sz) {
    snprintf(buf, sz, ODOO_MODEL_QN_PREFIX "%s", name ? name : "");
}

/* Upsert the Model node for `name`, returning its temp id (0 on failure).
 * Exported so call-resolution passes can pre-create model nodes before the
 * (multi-threaded) resolve phase reads them for ORM_CALLS edges. */
int64_t cbm_odoo_ensure_model_node(cbm_gbuf_t *gb, const char *name) {
    if (!gb || !name || !name[0]) {
        return 0;
    }
    char qn[ODOO_MODEL_QN_SIZE];
    cbm_odoo_model_qn(name, qn, sizeof(qn));
    const cbm_gbuf_node_t *existing = cbm_gbuf_find_by_qn(gb, qn);
    if (existing) {
        return existing->id;
    }
    return cbm_gbuf_upsert_node(gb, "Model", name, qn, "", 0, 0, "{}");
}

/* Ensure Model nodes exist for a class def's _name + every _inherit target.
 * Called single-threaded at definition/registry time so the resolve phase can
 * safely look them up by QN. */
void cbm_odoo_ensure_models_for_def(cbm_gbuf_t *gb, const char *model_name,
                                    const char **inherit_list) {
    if (model_name && model_name[0]) {
        cbm_odoo_ensure_model_node(gb, model_name);
    }
    if (inherit_list) {
        for (int i = 0; inherit_list[i]; i++) {
            cbm_odoo_ensure_model_node(gb, inherit_list[i]);
        }
    }
}

void cbm_pipeline_pass_odoo_model(cbm_pipeline_ctx_t *ctx) {
    if (!ctx || !ctx->gbuf) {
        return;
    }
    const cbm_gbuf_node_t **classes = NULL;
    int count = 0;
    if (cbm_gbuf_find_by_label(ctx->gbuf, "Class", &classes, &count) != 0 || !classes) {
        return;
    }

    int models = 0, defines = 0, inherits = 0;
    for (int i = 0; i < count; i++) {
        const cbm_gbuf_node_t *cls = classes[i];
        if (!cls || !cls->properties_json) {
            continue;
        }
        yyjson_doc *doc = yyjson_read(cls->properties_json, strlen(cls->properties_json), 0);
        if (!doc) {
            continue;
        }
        yyjson_val *root = yyjson_doc_get_root(doc);
        const char *model_name = yyjson_get_str(yyjson_obj_get(root, "odoo_model_name"));
        yyjson_val *inh = yyjson_obj_get(root, "odoo_inherit_list");
        bool has_inherit = inh && yyjson_is_arr(inh) && yyjson_arr_size(inh) > 0;

        if (!model_name && !has_inherit) {
            yyjson_doc_free(doc);
            continue;
        }

        /* The model this class "owns": its _name, or (extension class) each
         * inherited model. Establish the owning model node + DEFINES_MODEL. */
        int64_t own_model_id = 0;
        if (model_name && model_name[0]) {
            own_model_id = cbm_odoo_ensure_model_node(ctx->gbuf, model_name);
            if (own_model_id > 0) {
                models++;
                if (cbm_gbuf_insert_edge(ctx->gbuf, cls->id, own_model_id, "DEFINES_MODEL", "{}")) {
                    defines++;
                }
            }
        }

        if (has_inherit) {
            size_t idx, max;
            yyjson_val *v;
            yyjson_arr_foreach(inh, idx, max, v) {
                const char *parent = yyjson_get_str(v);
                if (!parent || !parent[0]) {
                    continue;
                }
                int64_t parent_id = cbm_odoo_ensure_model_node(ctx->gbuf, parent);
                if (parent_id <= 0) {
                    continue;
                }
                models++;
                if (own_model_id > 0) {
                    /* class declares its own model that extends `parent` */
                    if (cbm_gbuf_insert_edge(ctx->gbuf, own_model_id, parent_id, "INHERITS_MODEL",
                                             "{}")) {
                        inherits++;
                    }
                } else {
                    /* pure-extension class: it contributes methods to `parent` */
                    if (cbm_gbuf_insert_edge(ctx->gbuf, cls->id, parent_id, "DEFINES_MODEL", "{}")) {
                        defines++;
                    }
                }
            }
        }
        yyjson_doc_free(doc);
    }

    cbm_log_info("pass.odoo_model", "models", itoa_buf(models), "defines_model", itoa_buf(defines),
                 "inherits_model", itoa_buf(inherits));
}
