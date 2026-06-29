#!/usr/bin/env python3
"""Odoo-aware regression suite for codebase-memory-mcp.

Indexes the controlled fixture (test-infrastructure/odoo/fixture/odoo_library)
whose every cross-reference has a KNOWN expected graph fact, then asserts the
indexer found Odoo-specific syntax correctly. Checks are tagged by tier:

  A  language-scoped resolution (no Python->JS/XML false edges)   [done]
  B  Odoo model graph: Model nodes, INHERITS, env['x'] resolution [in progress]
  C  XML view inheritance + cross-layer links                     [planned]

Usage: verify_odoo.py [--bin PATH] [--repo FIXTURE_DIR] [--tier A|B|C|all]
Exit 0 only if all NON-pending checks pass.
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_BIN = os.path.join(ROOT, "build", "c", "codebase-memory-mcp")
DEFAULT_FIXTURE = os.path.join(HERE, "fixture", "odoo_library")


def run_tool(binp, tool, payload, cache):
    env = dict(os.environ, CBM_CACHE_DIR=cache)
    p = subprocess.run([binp, "cli", tool, json.dumps(payload)],
                       capture_output=True, text=True, env=env)
    try:
        return json.loads(p.stdout)
    except Exception:
        return {"_raw": p.stdout, "_err": p.stderr}


class Suite:
    def __init__(self, binp, repo):
        self.binp = binp
        self.repo = repo
        self.cache = tempfile.mkdtemp(prefix="cbm-odoo-verify-")
        self.project = None
        self.results = []  # (tier, name, status, detail)

    def index(self):
        run_tool(self.binp, "index_repository", {"repo_path": self.repo}, self.cache)
        projs = run_tool(self.binp, "list_projects", {}, self.cache).get("projects", [])
        self.project = projs[0]["name"] if projs else None
        return self.project is not None

    def search(self, **kw):
        kw.setdefault("project", self.project)
        kw.setdefault("limit", 500)
        return run_tool(self.binp, "search_graph", kw, self.cache).get("results", [])

    def trace(self, fn, depth=2):
        return run_tool(self.binp, "trace_path",
                        {"project": self.project, "function_name": fn, "depth": depth}, self.cache)

    def query(self, cypher):
        return run_tool(self.binp, "query_graph", {"project": self.project, "query": cypher}, self.cache)

    def record(self, tier, name, status, detail=""):
        self.results.append((tier, name, status, detail))

    # ---- checks -------------------------------------------------------
    def check_A_no_cross_lang(self):
        """A: Python controller method must not resolve calls to JS/XML nodes."""
        t = self.trace("borrow_for")
        callees = t.get("callees", [])
        false = [c for c in callees
                 if ".static." in c.get("qualified_name", "") or ".views." in c.get("qualified_name", "")]
        if false:
            self.record("A", "no cross-language false edges", "FAIL",
                        "; ".join(c["qualified_name"] for c in false))
        else:
            self.record("A", "no cross-language false edges", "PASS",
                        f"{len(callees)} callees, 0 cross-lang")

    def check_B_model_nodes(self):
        """B: Model nodes exist for declared _name values."""
        models = self.search(label="Model", limit=500)
        names = {m.get("name") for m in models}
        want = {"library.book", "library.member"}
        got = want & names
        if want <= names:
            self.record("B", "Model nodes for _name", "PASS", f"found {sorted(got)}")
        else:
            self.record("B", "Model nodes for _name", "FAIL" if models else "PENDING",
                        f"have={sorted(names)[:8]} want={sorted(want)}")

    def check_B_inherits(self):
        """B: prototype inheritance (_name + _inherit) yields INHERITS_MODEL edge."""
        r = self.query("MATCH (a)-[:INHERITS_MODEL]->(b) WHERE b.name = 'library.book' "
                       "RETURN a.name, b.name")
        rows = r.get("rows", r.get("results", []))
        ok = any("library.book.archive" in str(row) for row in rows)
        self.record("B", "INHERITS_MODEL edge (_name+_inherit)", "PASS" if ok else "PENDING",
                    json.dumps(rows)[:160])

    def check_B_extension(self):
        """B: extension (_inherit only) yields DEFINES_MODEL to the extended model."""
        r = self.query("MATCH (a)-[:DEFINES_MODEL]->(m) WHERE m.name = 'res.partner' RETURN a.name")
        rows = r.get("rows", r.get("results", []))
        ok = any("ResPartnerLibrary" in str(row) for row in rows)
        self.record("B", "DEFINES_MODEL edge (_inherit extension)", "PASS" if ok else "PENDING",
                    json.dumps(rows)[:160])

    def check_B_env_create_resolves(self):
        """B: self.env['library.member'].create() resolves to library.member's create."""
        t = self.trace("borrow_for")
        callees = t.get("callees", [])
        # success = a 'create' callee whose QN is the class implementing library.member
        good = [c for c in callees
                if c.get("name") == "create" and "LibraryMember" in c.get("qualified_name", "")]
        if good:
            self.record("B", "env['x'].create resolves to model", "PASS", good[0]["qualified_name"])
        else:
            creates = [c["qualified_name"] for c in callees if c.get("name") == "create"]
            self.record("B", "env['x'].create resolves to model", "PENDING",
                        f"create-> {creates}")

    def check_B_env_search_resolves(self):
        """B: request.env['library.book'].search() (base verb, not overridden) →
        ORM_CALLS edge to the library.book Model node."""
        r = self.query("MATCH (a)-[:ORM_CALLS]->(m) WHERE m.name = 'library.book' RETURN a.name")
        rows = r.get("rows", r.get("results", []))
        ok = any("list_books" in str(row) for row in rows)
        self.record("B", "env['x'].search → ORM_CALLS Model", "PASS" if ok else "PENDING",
                    json.dumps(rows)[:160])

    def check_C_view_inherit(self):
        """C: inherited view linked to its parent view (inherit_id)."""
        r = self.query("MATCH (a)-[:EXTENDS|INHERITS]->(b) WHERE a.name CONTAINS 'inherit' RETURN a.name, b.name")
        rows = r.get("rows", r.get("results", []))
        self.record("C", "view inherit_id edge", "PASS" if rows else "PENDING", json.dumps(rows)[:120])

    def check_C_view_model_link(self):
        """C: a view for model 'library.book' links to the library.book Model node."""
        r = self.query("MATCH (v)-[:FOR_MODEL|REFERENCES]->(m) WHERE m.name = 'library.book' RETURN v.name")
        rows = r.get("rows", r.get("results", []))
        self.record("C", "view->model cross-layer link", "PASS" if rows else "PENDING", json.dumps(rows)[:120])

    def run(self, tier):
        checks = {
            "A": [self.check_A_no_cross_lang],
            "B": [self.check_B_model_nodes, self.check_B_inherits, self.check_B_extension,
                  self.check_B_env_create_resolves, self.check_B_env_search_resolves],
            "C": [self.check_C_view_inherit, self.check_C_view_model_link],
        }
        tiers = ["A", "B", "C"] if tier == "all" else [tier]
        for tr in tiers:
            for chk in checks[tr]:
                try:
                    chk()
                except Exception as e:  # noqa
                    self.record(tr, chk.__name__, "ERROR", repr(e))


def main():
    args = sys.argv[1:]
    binp, repo, tier = DEFAULT_BIN, DEFAULT_FIXTURE, "all"
    i = 0
    while i < len(args):
        if args[i] == "--bin":
            binp = args[i + 1]; i += 2
        elif args[i] == "--repo":
            repo = args[i + 1]; i += 2
        elif args[i] == "--tier":
            tier = args[i + 1]; i += 2
        else:
            i += 1
    s = Suite(binp, repo)
    if not s.index():
        print("FATAL: indexing produced no project")
        return 2
    s.run(tier)
    icon = {"PASS": "✅", "FAIL": "❌", "PENDING": "⏳", "ERROR": "\U0001f4a5"}
    print(f"\n=== Odoo-aware suite ({s.project}) ===")
    nfail = 0
    for tr, name, status, detail in s.results:
        print(f"  [{tr}] {icon.get(status,'?')} {status:8} {name}")
        if detail and status != "PASS":
            print(f"          {detail}")
        if status in ("FAIL", "ERROR"):
            nfail += 1
    npass = sum(1 for r in s.results if r[2] == "PASS")
    npend = sum(1 for r in s.results if r[2] == "PENDING")
    print(f"\n  {npass} pass, {nfail} fail, {npend} pending (pending = tier not yet implemented)")
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
