# Odoo-aware fork of codebase-memory-mcp

This fork teaches the indexer Odoo's runtime conventions, which are invisible to
plain static AST analysis. Upstream sees Python classes and bare method calls;
this fork additionally understands Odoo **models**, **model inheritance**, **ORM
calls**, and **view inheritance**.

Integration branch: **`odoo-main`** (default). Each tier is a feature branch
merged back: `odoo-aware` (A), `tier-b-odoo-model-graph` (B), `tier-c-odoo-views` (C).

## What it adds

### Tier A — language-scoped call resolution
Upstream resolved a bare callee name against a global by-name index with no
language awareness, so a Python `self.env['x'].create()` could resolve to a
same-named JavaScript/XML node. The registry now records each definition's
language and `cbm_registry_resolve_lang` drops cross-language candidates.
*Result: Python ORM verbs no longer produce false edges to JS/XML.*

### Tier B — model graph + ORM call resolution
- Extracts `_name` / `_inherit` / `_inherits` from Python class bodies.
- **Model** nodes (`__model__<name>`), **DEFINES_MODEL** (class → model it
  declares/extends), **INHERITS_MODEL** (model → `_inherit` parent).
- ORM calls `self.env['x'].method()` / `request.env['x'].method()` resolve to
  the method on model x's class chain when overridden; otherwise an
  **ORM_CALLS** edge links the caller to the x **Model** node ("what models does
  this function touch").

### Tier C — view graph
- Scans `<record model="ir.ui.view">` XML records → **View** nodes.
- **FOR_MODEL** (view → its model) and **EXTENDS** (inherited view → parent via
  `inherit_id`, incl. cross-module `module.view_id` refs).

## New graph labels / edges

| Kind  | Name            | Meaning                                            |
|-------|-----------------|----------------------------------------------------|
| node  | `Model`         | an Odoo model (`_name`)                             |
| node  | `View`          | an `ir.ui.view` record                              |
| edge  | `DEFINES_MODEL` | Class → Model it declares or extends               |
| edge  | `INHERITS_MODEL`| Model → `_inherit` parent model                    |
| edge  | `ORM_CALLS`     | function → Model it queries via `env['x']`          |
| edge  | `FOR_MODEL`     | View → the model it renders                         |
| edge  | `EXTENDS`       | View → parent View (`inherit_id`)                   |

Labels/edges are discovered dynamically, so they appear automatically in
`get_graph_schema`, `search_graph`, and `query_graph`. To traverse the new edge
types in `trace_path`, pass `edge_types` explicitly, e.g.
`edge_types=["CALLS","ORM_CALLS","INHERITS_MODEL"]`.

## Example queries

```cypher
-- which functions touch a model
MATCH (f)-[:ORM_CALLS]->(m) WHERE m.name = 'hr.attendance' RETURN f.name

-- a model's inheritance chain
MATCH (a)-[:INHERITS_MODEL]->(b) RETURN a.name, b.name

-- views of a model and their inheritance
MATCH (v)-[:FOR_MODEL]->(m) WHERE m.name = 'library.book' RETURN v.name
MATCH (v)-[:EXTENDS]->(p) RETURN v.name, p.name
```

## Tests

A controlled fixture with known expected graph facts plus a tiered checker:

```bash
scripts/build.sh
python3 test-infrastructure/odoo/verify_odoo.py     # 8/8 across tiers A,B,C
make -f Makefile.cbm test                           # 5714 upstream tests
```

## Implementation map

| Area | Files |
|------|-------|
| Language-scoped resolution | `src/pipeline/registry.c`, `pipeline.h`, `pass_calls.c`, `pass_parallel.c` |
| Odoo attribute extraction | `internal/cbm/extract_defs.c` (`extract_odoo_class_attributes`), `extract_calls.c` (`extract_python_env_model`), `cbm.h` |
| Model index + ORM resolve | `src/pipeline/registry.c` (`cbm_registry_add_model`, `cbm_registry_resolve_orm`) |
| Model nodes / edges | `src/pipeline/pass_odoo_model.c` |
| View graph | `src/pipeline/pass_odoo_xml.c` |

## Known limitations / follow-ups

- ORM receiver shape is `(self|request|cls).env['literal'].method()`. Chained or
  computed receivers (`env['x'].sudo().create()`, `env[var]`) fall back to
  normal resolution.
- Incremental re-index rebuilds the model graph from changed files; a full
  reindex is the tested path.
- View `inherit_id` cross-module refs are stored as the literal ref string
  (`module.view_id`), not resolved to the parent record's node across modules.
- Security CSV / `ir.model.access` → model linkage is not yet emitted.
