#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
SENDER_BIN="${BUILD_DIR}/sender"
CSV_FILE="${CSV_FILE:-${ROOT_DIR}/examples/flows_example.csv}"

PORT_ID="${PORT_ID:-0}"
NB_VC="${NB_VC:-8}"
RING_SIZE="${RING_SIZE:-1024}"
PKT_SIZE="${PKT_SIZE:-8000}"
MEMPOOL_SIZE="${MEMPOOL_SIZE:-32768}"
TX_BURST="${TX_BURST:-64}"
TICK_US="${TICK_US:-1000}"

if [[ ! -x "${SENDER_BIN}" ]]; then
  echo "[run_sender] sender binary not found, building first..."
  cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
  cmake --build "${BUILD_DIR}" -j
fi

if [[ ! -f "${CSV_FILE}" ]]; then
  echo "[run_sender] CSV file not found: ${CSV_FILE}" >&2
  exit 1
fi

if [[ "$#" -lt 1 ]]; then
  cat <<'EOF'
Usage:
  scripts/run_sender.sh "<EAL_ARGS>"

Example:
  scripts/run_sender.sh "-l 2,3,4 -n 4 --vdev=net_tap0,iface=tap0"

Environment variables (optional):
  CSV_FILE     default: examples/flows_example.csv
  PORT_ID      default: 0
  NB_VC        default: 8
  RING_SIZE    default: 1024
  PKT_SIZE     default: 8000
  MEMPOOL_SIZE default: 32768
  TX_BURST     default: 64
  TICK_US      default: 1000
EOF
  exit 1
fi

EAL_ARGS="$1"

set -x
"${SENDER_BIN}" ${EAL_ARGS} -- \
  --csv "${CSV_FILE}" \
  --port "${PORT_ID}" \
  --vcs "${NB_VC}" \
  --ring-size "${RING_SIZE}" \
  --pkt-size "${PKT_SIZE}" \
  --mempool "${MEMPOOL_SIZE}" \
  --tx-burst "${TX_BURST}" \
  --tick-us "${TICK_US}"
