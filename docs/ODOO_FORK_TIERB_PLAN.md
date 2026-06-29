# Tier B Implementation Plan (auto-generated from investigation workflow)

I have verified the full architecture (extraction → registry → resolution, both sequential and parallel paths, plus the incremental seed). Here is the plan.

---

# Tier B Implementation Plan — Odoo Model Graph + ORM Call Resolution

## Verified architecture (grounding)

- **Call resolution** runs per-file in two mirror functions: `resolve_single_call()` (`src/pipeline/pass_calls.c:420`, lang-scoped resolve at `:475`) and `resolve_file_calls()` (`src/pipeline/pass_parallel.c:1744`, lang-scoped resolve at `:1846`). Both call `cbm_registry_resolve_lang()` and then `cbm_gbuf_find_by_qn()` + `emit_classified_edge()`.
- **Registry** (`src/pipeline/registry.c:78-90`) holds three hash tables: `exact` (qn→label), `by_name` (simpleName→`qn_array_t`), `lang_of_qn` (qn→lang+1). Public API in `src/pipeline/pipeline.h:142-236`.
- **Registry is fully built before any call resolution** in all three paths: `process_def()` (`pass_definitions.c:288`, calls `cbm_registry_add` at `:312`), `register_and_link_def()` (`pass_parallel.c:828`, called from `cbm_build_registry_from_cache` `:938`), and the incremental seed visitor (`pipeline_incremental.c:503-530`). These are the **three KEEP-IN-SYNC points**.
- **Method QN scheme** (Python): class node QN = `cbm_fqn_compute(project, rel, ClassName)` (`extract_defs.c:3628`); a method's QN = `"<class_qn>.<method_name>"` (`extract_defs.c:3892`) with `parent_class = class_qn`. So given a class QN, a method `create` is the registry-`exact` key `"<class_qn>.create"`.
- **Node/edge creation**: `cbm_gbuf_upsert_node()` (dedup by QN), `cbm_gbuf_insert_edge()` (dedup by src+tgt+type). Property JSON built by `build_def_props()` — duplicated in `pass_definitions.c:217-287` AND `pass_parallel.c:262-331` (KEEP IN SYNC).
- **Build**: `./scripts/build.sh`. **Tests**: `tests/test_registry.c`, `tests/test_extraction.c`, `tests/test_extraction_inheritance.c`.

## Key design decision

ORM call resolution is driven by an **in-memory model index inside the registry** (consumed in the resolve pass), NOT by graph traversal. The **Model nodes (goal a)** and **INHERITS_MODEL edges (goal b)** are a separate, queryable *output* for `trace_path`. Both consume the same extracted fields (`_name`, `_inherit`). This decouples "make ORM edges correct" (MVP) from "make the model graph browsable".

---

## Step 1 — Add `model_name` to CBMCall  (`internal/cbm/cbm.h`)
- Edit the `CBMCall` struct (ends at the `bool is_method;` line ~244). Add:
  ```c
  const char *model_name;  // Odoo: model from self.env['x'] receiver subscript, or NULL
  ```
- **Connects to:** Step 2 populates it; Step 6 reads it.
- **Verify:** `./scripts/build.sh` still green (field unused, defaults NULL via `{0}` initializers already used at `pass_calls.c` and `extract_calls.c`).

## Step 2 — Extract the receiver model name in the Python call extractor  (`internal/cbm/extract_calls.c`)
- Add a static helper `extract_python_env_model()` modeled on the existing dict-dispatch block (`:1012-1023`) and reusing `strip_quotes()` (`:50-59`). Walk: `call.function` is `attribute` → its `value` field is a `subscript` → subscript `value` field is `attribute`/`identifier` whose text ends in `env` → subscript `subscript` field is a `string` literal. Return the dequoted string.
- Call it inside `handle_calls()` (`:1869`) right after `callee_name` is extracted, gated on `ctx->language == CBM_LANG_PYTHON`, and assign `call.model_name` before `cbm_calls_push`.
- **Gotchas (from findings):** keep the `string`-only gate on the subscript index (skip `env[var]`); accept BOTH `identifier` and `attribute` as the subscript `value` so `request.env['x']` works, not just `self.env['x']`; do NOT set `is_method` (Perl-only flag).
- **Connects to:** Step 6 reads `call.model_name`.
- **Verify:** add a case to `tests/test_extraction.c` asserting that for `self.env['hr.leave'].create(vals)` the emitted `CBMCall` has `callee_name=="create"` and `model_name=="hr.leave"`.

## Step 3 — Add Odoo fields to CBMDefinition  (`internal/cbm/cbm.h`)
- Edit `CBMDefinition` (`:178-217`). Add after `base_classes`:
  ```c
  const char *odoo_model_name;     // value of _name, or NULL
  const char **odoo_inherit_list;  // NULL-terminated array from _inherit / _inherits keys, or NULL
  ```
- Mirror the existing NULL-terminated-array convention of `base_classes`/`decorators`.
- **Verify:** build green (unset everywhere → NULL).

## Step 4 — Extract `_name` / `_inherit` / `_inherits` from the class body  (`internal/cbm/extract_defs.c`)
- Add `extract_odoo_class_attributes(ctx, class_node, &def)` and call it from `extract_class_def()` after the Class def is finalized (after `extract_class_variables(ctx, node, spec)` at `:3652`, while `def` is still in scope and before push).
- Use the existing `find_class_body(node, ctx->language)` (`:3705`) to get the `block`, iterate `ts_node_named_child`, match `assignment` nodes whose `left` field text is `_name`/`_inherit`/`_inherits`, read the `right` field:
  - `_name`: dequote string → `def.odoo_model_name`.
  - `_inherit` string → single-element list; `_inherit` `list` → iterate elements (dequote each).
  - `_inherits` `dictionary` → iterate pairs, take each `pair` `key` (dequoted) into the list.
- Reuse the quote-stripping pattern from `extract_unified.c:525-532` and `cbm_node_text()`.
- **Gotchas:** Python class body is a `block` with `expression_statement → assignment` children; assignment fields are `left`/`right` (not name/value); ensure the array is NULL-terminated (arena-allocated) or `append_json_str_array` (Step 7) and the model-index loop (Step 5) overrun.
- **Connects to:** Step 5 (index build) and Step 7 (node/edge output) both read these fields.
- **Verify:** add cases to `tests/test_extraction_inheritance.c` for `_name`, `_inherit='a'`, `_inherit=['a','b']`, `_inherits={'a.b':'x'}`.

## Step 5 — Model index in the registry + populate at all 3 build sites  (`src/pipeline/registry.c`, `pipeline.h`, + 3 call sites)
- In `struct cbm_registry` (`registry.c:78`) add two `CBMHashTable*`:
  - `model_contributors` — modelName → `qn_array_t*` (every Class QN whose `_name`==m OR whose `_inherit` contains m).
  - `model_inherits` — modelName → `qn_array_t*` (parent model NAMES from `_inherit`/`_inherits`).
  - Create in `cbm_registry_new()` (`:485`), free in `cbm_registry_free()` (`:521`, reuse `free_qn_array`).
- New public API in `pipeline.h` (near `:149`):
  ```c
  void cbm_registry_add_model(cbm_registry_t *r, const char *model_name,
                              const char *class_qn, const char **inherit_list);
  ```
  Implementation appends `class_qn` to `model_contributors[model_name]`, and for each inherit target appends to both `model_inherits[model_name]` and `model_contributors[inherit_target]` (so an `_inherit`-only extension class is registered as a contributor of the model it extends).
- **Call `cbm_registry_add_model` at the same 3 KEEP-IN-SYNC sites** that call `cbm_registry_add`, guarded on `def->odoo_model_name || def->odoo_inherit_list`:
  1. `process_def()` (`pass_definitions.c:312` area) — pass `def->qualified_name` (the class QN) + the two odoo fields.
  2. `register_and_link_def()` (`pass_parallel.c:828` area).
  3. **Incremental** (`pipeline_incremental.c:503-530`): this seeds from existing **gbuf nodes**, not from `CBMDefinition`, so it has no `odoo_*` fields. Reconstruct the model index from the node's persisted `properties_json` (the `odoo_model_name`/`odoo_inherit_list` written in Step 7) by parsing it in the foreach-node callback. **This makes Step 7's property serialization a hard dependency for incremental correctness** — see Risk 2.
- **Verify:** unit test in `tests/test_registry.c`: add two models, assert `model_contributors`/`model_inherits` contents.

## Step 6 — ORM resolution + wire into BOTH resolve paths  ← **MVP COMPLETES HERE**
- Add to `registry.c` + `pipeline.h`:
  ```c
  cbm_resolution_t cbm_registry_resolve_orm(const cbm_registry_t *r,
                                            const char *model_name, const char *method_name);
  ```
  BFS from `model_name` (visited-set to guard `_inherit` cycles): for the current model, for each QN in `model_contributors[model]`, probe `exact` for `"<qn>.<method_name>"` (build into a stack `char[CBM_SZ_512]`); first hit returns `{qualified_name=stored_key, strategy="odoo_orm", confidence=0.9, candidate_count=1}`. If none, enqueue `model_inherits[model]`. Return `empty_result()` if exhausted.
- **Sequential** — in `resolve_single_call()` (`pass_calls.c:420`), immediately before the `cbm_registry_resolve_lang` call (`:475`):
  ```c
  if (lang == CBM_LANG_PYTHON && call->model_name && call->model_name[0]) {
      cbm_resolution_t orm = cbm_registry_resolve_orm(ctx->registry, call->model_name, call->callee_name);
      if (orm.qualified_name && orm.qualified_name[0]) {
          const cbm_gbuf_node_t *t = cbm_gbuf_find_by_qn(ctx->gbuf, orm.qualified_name);
          if (t && source_node->id != t->id) {
              emit_classified_edge(ctx, call, source_node, t, &orm, module_qn, imp_keys, imp_vals, imp_count);
          }
          return SKIP_ONE;   // model_name present => do NOT fall through to bare-name guessing
      }
      return SKIP_ONE;       // see Risk 3: decide drop-vs-fallthrough
  }
  ```
- **Parallel** — replicate the identical branch in `resolve_file_calls()` (`pass_parallel.c:1744`) just before its `cbm_registry_resolve_lang` (`:1846`), writing the edge through the worker's `emit` path used there.
- **Connects to:** this is the correctness payoff — `self.env['hr.leave'].create()` now resolves to `<hr.leave class qn>.create`.
- **Verify (MVP acceptance):** index a tiny fixture (`class HrLeave(models.Model): _name='hr.leave'; def create(self,v): ...` + a caller doing `self.env['hr.leave'].create({})`); assert a `CALLS` edge from the caller to `...HrLeave.create` and that it does NOT point at any same-named non-Python node. Add as an integration case alongside `test_registry.c`/`test_extraction.c`.

> **Minimum viable subset = Steps 1, 2, 3, 4, 5, 6** (with the incremental sub-case of Step 5 deferred — full reindex works without Step 7; see Risk 2).

## Step 7 — Emit "Model" nodes (goal a) + INHERITS_MODEL edges (goal b)
- **Serialize odoo fields** in `build_def_props()` — **BOTH** `pass_definitions.c:217-287` and `pass_parallel.c:262-331` (use `append_json_string` for `odoo_model_name`, `append_json_str_array` for `odoo_inherit_list`). This also unblocks Step 5's incremental rebuild.
- **Create Model nodes + edges** in a small helper invoked from `process_def()` (sequential) and from the parallel edge phase (alongside `resolve_def_inherits()` at `pass_parallel.c:2039-2057`, which already creates plain `INHERITS` edges from `base_classes` — add the Odoo variant next to it so it lands in the same `local_edge_buf`):
  - Synthetic Model node, deterministic QN `"__model__<model_name>"` (mirrors the `__route__` scheme in `pass_route_nodes.c:194-215`): `cbm_gbuf_upsert_node(gbuf, "Model", model_name, "__model__"+model_name, file_path, 0,0, props)` (dedup across the many classes contributing to one model).
  - For each `_inherit` target T: `cbm_gbuf_insert_edge(gbuf, "__model__<m>" node, "__model__<T>" node, "INHERITS_MODEL", "{}")`.
  - Optionally a `DEFINES_MODEL` edge from the Class node to its Model node.
- **Gotchas:** uppercase+underscore edge type (`INHERITS_MODEL`) is required by `mcp.c:1276-1286` validation; parallel workers must write to `local_edge_buf` and rely on QN-dedup at merge; incremental won't recreate nodes for unchanged files (use the backfill pattern noted in `pass_route_nodes.c`).
- **Verify:** dump to SQLite, `SELECT * FROM nodes WHERE label='Model'` and `SELECT * FROM edges WHERE type='INHERITS_MODEL'`.

## Step 8 — Expose for queries: trace_path mode + schema + docs
- Labels/edge types are **auto-discovered** (`store.c:3053-3172` GROUP BY) — no allow-list edit needed for `get_graph_schema`.
- `trace_path` only follows edge types you pass. In `resolve_trace_edge_types()` (`mcp.c:2350-2396`) add an `mode="odoo"` default = `["INHERITS_MODEL","CALLS"]` (and document that callers can pass `edge_types` explicitly).
- **Verify:** `trace_path` from a caller over `CALLS` lands on the model method; over `INHERITS_MODEL` walks the model chain.

---

## Biggest risks

1. **Parallel/sequential drift (highest).** Every change in Steps 5, 6, 7 must land in BOTH `pass_calls.c`/`pass_definitions.c` AND `pass_parallel.c` (`resolve_file_calls:1744`, `register_and_link_def:828`, `build_def_props:262`). The codebase explicitly marks these "KEEP IN SYNC"; a one-sided edit makes parallel vs sequential indexes diverge silently. Diff the two `build_def_props` and the two resolve functions after each step.
2. **Incremental re-resolution has no CBMDefinition.** `pipeline_incremental.c:503-530` rebuilds the registry from gbuf nodes only. The model index (Step 5) therefore must be reconstructable from persisted `properties_json` (Step 7), so for a clean MVP either (a) ship Step 7's serialization together with Step 5, or (b) explicitly scope MVP to full reindex and flag incremental ORM edges as a follow-up. Recommend (a).
3. **Fallthrough policy when ORM lookup misses.** Base ORM methods (`search`/`write`/`browse`) live in un-indexed `odoo/odoo`. When `model_name` is set but no contributor defines the method, do **not** fall through to `cbm_registry_resolve_lang` (that re-introduces the bare-name guess Tier A removed). Return `SKIP_ONE` (drop) — a missing-but-correct edge beats a wrong one. Make this explicit and test it.
4. **Receiver pattern brittleness.** Step 2 only handles `(self|request).env['literal'].method()`. Chained/computed receivers (`self.env['x'].sudo().create()`, `env[var]`) won't carry `model_name` and silently fall back to normal resolution — acceptable, but document the supported shape so it isn't mistaken for a bug.
5. **QN collision on synthetic Model nodes.** `cbm_gbuf_upsert_node` dedups by QN only; the `__model__` prefix must be collision-free vs real class QNs and vs `__route__` (it is). Empty `model_name` must be rejected before upsert (NULL-deref downstream).

## Files touched (summary)
- `internal/cbm/cbm.h` — CBMCall + CBMDefinition fields (Steps 1, 3)
- `internal/cbm/extract_calls.c` — receiver model extraction (Step 2)
- `internal/cbm/extract_defs.c` — `_name`/`_inherit`/`_inherits` extraction (Step 4)
- `src/pipeline/registry.c` + `src/pipeline/pipeline.h` — model index + `cbm_registry_add_model` + `cbm_registry_resolve_orm` (Steps 5, 6)
- `src/pipeline/pass_definitions.c` — `process_def` model-index call, `build_def_props` serialization, Model node/edge (Steps 5, 6, 7)
- `src/pipeline/pass_parallel.c` — twins of all the above: `register_and_link_def`, `resolve_file_calls`, `build_def_props`, `resolve_def_inherits` area (Steps 5, 6, 7) **[primary sync risk]**
- `src/pipeline/pipeline_incremental.c` — model-index rebuild from node properties (Step 5 incremental)
- `src/mcp/mcp.c` — `odoo` trace mode (Step 8)
- `tests/test_extraction.c`, `tests/test_extraction_inheritance.c`, `tests/test_registry.c` — per-step verification

Each step builds independently with `./scripts/build.sh`; MVP acceptance (`self.env['hr.leave'].create()` → `HrLeave.create` CALLS edge) is reached at the end of Step 6.