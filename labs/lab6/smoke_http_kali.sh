#!/usr/bin/env bash
set -euo pipefail

BASE="${1:-http://127.0.0.1:8080}"

echo "== current =="
curl -sS "$BASE/api/current" || true
echo

FROM="$(python3 - <<'PY'
from datetime import datetime, timezone, timedelta
now = datetime.now(timezone.utc)
print((now - timedelta(hours=1)).strftime("%Y-%m-%dT%H:%M:%SZ"))
PY
)"
TO="$(python3 - <<'PY'
from datetime import datetime, timezone
now = datetime.now(timezone.utc)
print(now.strftime("%Y-%m-%dT%H:%M:%SZ"))
PY
)"

echo "== stats =="
curl -sS "$BASE/api/stats?from=$FROM&to=$TO" || true
echo
