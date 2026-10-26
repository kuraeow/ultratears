#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PYTHONUNBUFFERED=1

if ! command -v python3 >/dev/null 2>&1; then
    echo "ERROR: python3 not found on PATH. Please install Python 3.10+." >&2
    exit 1
fi

exec python3 "$ROOT/tools/setup.py" "$@"
