#!/bin/bash
# Odoo-aware regression check: trace a Python controller method and report
# cross-language false edges (Python call resolving to JS/XML nodes).
# Usage: verify_trace.sh [module_path]
set -uo pipefail
BIN="$(cd "$(dirname "$0")/../.." && pwd)/build/c/codebase-memory-mcp"
REPO="${1:-/Users/stanleykao72/Documents/odoo_workshop/odoo18ee_esmith_2026/user/job_field_recorder}"
export CBM_CACHE_DIR="$(mktemp -d)"
$BIN cli index_repository "{\"repo_path\":\"$REPO\"}" >/dev/null 2>&1
P=$(echo "$REPO" | sed 's#^/##; s#/#-#g; s#_#_#g')
P=$($BIN cli list_projects '{}' 2>/dev/null | python3 -c "import json,sys;print(json.load(sys.stdin)['projects'][0]['name'])")
echo "project: $P"
$BIN cli trace_path "{\"project\":\"$P\",\"function_name\":\"save_cell\",\"depth\":2}" 2>/dev/null | python3 -c "
import json,sys
d=json.load(sys.stdin)
cees=d.get('callees',[])
false_edges=[c for c in cees if any(x in c['qualified_name'] for x in ['.static.','.views.']) ]
print(f'save_cell callees total: {len(cees)}')
print(f'cross-lang FALSE edges (→ static JS / views XML): {len(false_edges)}')
for c in false_edges: print('  ✗', c['name'], '→', c['qualified_name'].split('job_field_recorder.')[-1])
"
rm -rf "$CBM_CACHE_DIR"
