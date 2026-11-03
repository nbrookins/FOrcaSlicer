#!/bin/bash
# Clang 17+ requires explicit <> on dependent template calls (NodeManager.h).
set -euo pipefail
ROOT="${1:-.}"
FILE="$ROOT/openvdb/openvdb/tree/NodeManager.h"
if [[ ! -f "$FILE" ]]; then
    echo "patch-clang21: $FILE not found" >&2
    exit 1
fi
python3 - "$FILE" <<'PY'
import sys
path = sys.argv[1]
text = open(path, encoding="utf-8").read()
old = "OpT::template eval("
new = "OpT::template eval<>("
if old not in text:
    if new in text:
        sys.exit(0)
    print("patch-clang21: pattern not found", file=sys.stderr)
    sys.exit(1)
count = text.count(old)
text = text.replace(old, new)
open(path, "w", encoding="utf-8").write(text)
print(f"patch-clang21: updated {count} site(s) in {path}")
PY
