#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

PIPELINE="${1:-cbfc}"
PROFILE="${2:-tasks/profiles/cbfc_default.env}"

case "${PIPELINE}" in
  cbfc)
    PIPELINE_SCRIPT="${ROOT_DIR}/tasks/pipelines/cbfc.sh"
    ;;
  *)
    echo "[tasks] unknown pipeline: ${PIPELINE}" >&2
    echo "[tasks] available pipelines: cbfc" >&2
    exit 1
    ;;
esac

exec bash "${PIPELINE_SCRIPT}" "${PROFILE}"
