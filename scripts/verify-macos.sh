#!/usr/bin/env bash
# Python owns native suite selection, preflight, logs and receipts.
set -euo pipefail
mode="${1:-debug}"
if (( $# )); then shift; fi
verification_python="${CODESTUDIO_CONTRACT_PYTHON:-python3}"
exec "$verification_python" "$(dirname -- "${BASH_SOURCE[0]}")/verify.py" --mode "$mode" "$@"
