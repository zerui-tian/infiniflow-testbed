#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
RECEIVER_BIN="${BUILD_DIR}/receiver"

TASK_PROFILE="${TASK_PROFILE:-tasks/profiles/cbfc_default.env}"
if [[ "${TASK_PROFILE}" = /* ]]; then
  PROFILE_PATH="${TASK_PROFILE}"
else
  PROFILE_PATH="${ROOT_DIR}/${TASK_PROFILE}"
fi
if [[ -f "${PROFILE_PATH}" ]]; then
  # shellcheck source=/dev/null
  source "${PROFILE_PATH}"
fi

DEFAULT_EAL_LCORES="5"
DEFAULT_EAL_MEM_CHANNELS="4"
DEFAULT_EAL_PCI_ADDR="0000:01:00.1"
DEFAULT_LOG_LEVEL="DEBUG"
DEFAULT_PORT_ID="0"
DEFAULT_NB_VC="1"
DEFAULT_RX_BURST="64"
DEFAULT_TX_BURST="64"
DEFAULT_PKT_SIZE="1500"
DEFAULT_MEMPOOL_SIZE="32768"
DEFAULT_FC_MODE="cbfc"
DEFAULT_CBFC_BUFFER_PKTS="2"
DEFAULT_OUTPUT_FILE="${ROOT_DIR}/output/cbfc_test_receiver.csv"

EAL_LCORES="${EAL_LCORES:-${RECEIVER_EAL_LCORES:-$DEFAULT_EAL_LCORES}}"
EAL_MEM_CHANNELS="${EAL_MEM_CHANNELS:-${RECEIVER_EAL_MEM_CHANNELS:-$DEFAULT_EAL_MEM_CHANNELS}}"
EAL_PCI_ADDR="${EAL_PCI_ADDR:-${RECEIVER_EAL_PCI_ADDR:-$DEFAULT_EAL_PCI_ADDR}}"
LOG_LEVEL="${LOG_LEVEL:-${RECEIVER_LOG_LEVEL:-$DEFAULT_LOG_LEVEL}}"
PORT_ID="${PORT_ID:-${RECEIVER_PORT_ID:-$DEFAULT_PORT_ID}}"
NB_VC="${NB_VC:-${RECEIVER_NB_VC:-$DEFAULT_NB_VC}}"
RX_BURST="${RX_BURST:-${RECEIVER_RX_BURST:-$DEFAULT_RX_BURST}}"
TX_BURST="${TX_BURST:-${RECEIVER_TX_BURST:-$DEFAULT_TX_BURST}}"
PKT_SIZE="${PKT_SIZE:-${RECEIVER_PKT_SIZE:-$DEFAULT_PKT_SIZE}}"
MEMPOOL_SIZE="${MEMPOOL_SIZE:-${RECEIVER_MEMPOOL_SIZE:-$DEFAULT_MEMPOOL_SIZE}}"
FC_MODE="${FC_MODE:-${RECEIVER_FC_MODE:-$DEFAULT_FC_MODE}}"
CBFC_BUFFER_PKTS="${CBFC_BUFFER_PKTS:-${RECEIVER_CBFC_BUFFER_PKTS:-$DEFAULT_CBFC_BUFFER_PKTS}}"
RECEIVER_OUTPUT_FILE_REL="${RECEIVER_OUTPUT_FILE:-}"
if [[ -n "${RECEIVER_OUTPUT_FILE_REL}" ]]; then
  OUTPUT_FILE="${OUTPUT_FILE:-${ROOT_DIR}/${RECEIVER_OUTPUT_FILE_REL}}"
else
  OUTPUT_FILE="${OUTPUT_FILE:-$DEFAULT_OUTPUT_FILE}"
fi

to_dpdk_log_level() {
  local level="${1^^}"
  case "${level}" in
    EMERG) echo 1 ;;
    ALERT) echo 2 ;;
    CRIT) echo 3 ;;
    ERR|ERROR) echo 4 ;;
    WARNING|WARN) echo 5 ;;
    NOTICE) echo 6 ;;
    INFO) echo 7 ;;
    DEBUG) echo 8 ;;
    *)
      echo "[tasks/receiver] invalid LOG_LEVEL: ${1}" >&2
      exit 1
      ;;
  esac
}

DPDK_LOG_LEVEL="$(to_dpdk_log_level "${LOG_LEVEL}")"

if [[ ! -x "${RECEIVER_BIN}" ]]; then
  echo "[tasks/receiver] receiver binary not found, building first..."
  cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
  cmake --build "${BUILD_DIR}" -j
fi

mkdir -p "$(dirname "${OUTPUT_FILE}")"

echo "========================================"
echo "Start receiver (tasks)"
echo "========================================"
echo "PROFILE_PATH:   ${PROFILE_PATH}"
echo "EAL_LCORES:     ${EAL_LCORES}"
echo "EAL_MEM_CH:     ${EAL_MEM_CHANNELS}"
echo "EAL_PCI_ADDR:   ${EAL_PCI_ADDR:-<empty>}"
echo "LOG_LEVEL:      ${LOG_LEVEL} (${DPDK_LOG_LEVEL})"
echo "PORT_ID:        ${PORT_ID}"
echo "NB_VC:          ${NB_VC}"
echo "RX_BURST:       ${RX_BURST}"
echo "TX_BURST:       ${TX_BURST}"
echo "PKT_SIZE:       ${PKT_SIZE}"
echo "MEMPOOL_SIZE:   ${MEMPOOL_SIZE}"
echo "FC_MODE:        ${FC_MODE}"
echo "CBFC_BUFFER:    ${CBFC_BUFFER_PKTS}"
echo "OUTPUT_FILE:    ${OUTPUT_FILE}"
echo "========================================"
echo

EAL_OPTS=(
  -l "${EAL_LCORES}"
  -n "${EAL_MEM_CHANNELS}"
  --log-level "${DPDK_LOG_LEVEL}"
)
if [[ -n "${EAL_PCI_ADDR}" ]]; then
  EAL_OPTS+=(-a "${EAL_PCI_ADDR}")
fi

set -x
set +e
"${RECEIVER_BIN}" "${EAL_OPTS[@]}" -- \
  --port "${PORT_ID}" \
  --vcs "${NB_VC}" \
  --rx-burst "${RX_BURST}" \
  --tx-burst "${TX_BURST}" \
  --pkt-size "${PKT_SIZE}" \
  --mempool "${MEMPOOL_SIZE}" \
  --fc-mode "${FC_MODE}" \
  --cbfc-buffer-pkts "${CBFC_BUFFER_PKTS}" \
  --output "${OUTPUT_FILE}"
EXIT_CODE=$?
set -e
set +x

if [[ ${EXIT_CODE} -ne 0 ]]; then
  echo "[tasks/receiver] exited with code: ${EXIT_CODE}"
else
  echo "[tasks/receiver] exited normally"
fi

exit "${EXIT_CODE}"
