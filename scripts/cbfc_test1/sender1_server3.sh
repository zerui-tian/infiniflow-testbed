#!/usr/bin/env bash
set -euo pipefail

#
# 启动 sender 程序
#
# 直接运行:
#   ./scripts/run_sender.sh
# 可通过修改下方默认参数或导出同名环境变量覆盖。
#

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
SENDER_BIN="${BUILD_DIR}/sender"

# 默认参数（可直接运行）
DEFAULT_EAL_LCORES="1-8"
DEFAULT_EAL_MEM_CHANNELS="4"
DEFAULT_EAL_PCI_ADDR="0000:41:00.0"
DEFAULT_LOG_LEVEL="INFO"
DEFAULT_CSV_FILE="${ROOT_DIR}/examples/cbfc_test.csv"
# DEFAULT_CSV_FILE="${ROOT_DIR}/examples/flows_sgh.csv"
DEFAULT_PORT_ID="0"
DEFAULT_NB_VC="4"
DEFAULT_RING_SIZE="8192"
DEFAULT_PKT_SIZE="9000"
DEFAULT_MEMPOOL_SIZE="32768"
DEFAULT_TX_BURST="16"
DEFAULT_RX_BURST="64"
DEFAULT_TICK_US="1"
# DEFAULT_FC_MODE="none"
DEFAULT_FC_MODE="cbfc"
DEFAULT_INITIAL_FCCL="10"

EAL_LCORES="${EAL_LCORES:-$DEFAULT_EAL_LCORES}"
EAL_MEM_CHANNELS="${EAL_MEM_CHANNELS:-$DEFAULT_EAL_MEM_CHANNELS}"
EAL_PCI_ADDR="${EAL_PCI_ADDR:-$DEFAULT_EAL_PCI_ADDR}"
LOG_LEVEL="${LOG_LEVEL:-$DEFAULT_LOG_LEVEL}"
CSV_FILE="${CSV_FILE:-$DEFAULT_CSV_FILE}"
PORT_ID="${PORT_ID:-$DEFAULT_PORT_ID}"
NB_VC="${NB_VC:-$DEFAULT_NB_VC}"
RING_SIZE="${RING_SIZE:-$DEFAULT_RING_SIZE}"
PKT_SIZE="${PKT_SIZE:-$DEFAULT_PKT_SIZE}"
MEMPOOL_SIZE="${MEMPOOL_SIZE:-$DEFAULT_MEMPOOL_SIZE}"
TX_BURST="${TX_BURST:-$DEFAULT_TX_BURST}"
RX_BURST="${RX_BURST:-$DEFAULT_RX_BURST}"
TICK_US="${TICK_US:-$DEFAULT_TICK_US}"
FC_MODE="${FC_MODE:-$DEFAULT_FC_MODE}"
INITIAL_FCCL="${INITIAL_FCCL:-$DEFAULT_INITIAL_FCCL}"

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
      echo "[run_sender] invalid LOG_LEVEL: ${1} (use EMERG|ALERT|CRIT|ERROR|WARN|NOTICE|INFO|DEBUG)" >&2
      exit 1
      ;;
  esac
}

DPDK_LOG_LEVEL="$(to_dpdk_log_level "${LOG_LEVEL}")"

if [[ ! -x "${SENDER_BIN}" ]]; then
  echo "[run_sender] sender binary not found, building first..."
  cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
  cmake --build "${BUILD_DIR}" -j
fi

if [[ ! -f "${CSV_FILE}" ]]; then
  echo "[run_sender] CSV file not found: ${CSV_FILE}" >&2
  exit 1
fi

echo "========================================"
echo "Start sender"
echo "========================================"
echo "EAL_LCORES:    ${EAL_LCORES}"
echo "EAL_MEM_CH:    ${EAL_MEM_CHANNELS}"
echo "EAL_PCI_ADDR:  ${EAL_PCI_ADDR:-<empty>}"
echo "LOG_LEVEL:     ${LOG_LEVEL} (${DPDK_LOG_LEVEL})"
echo "CSV_FILE:      ${CSV_FILE}"
echo "PORT_ID:       ${PORT_ID}"
echo "NB_VC:         ${NB_VC}"
echo "RING_SIZE:     ${RING_SIZE}"
echo "PKT_SIZE:      ${PKT_SIZE}"
echo "MEMPOOL_SIZE:  ${MEMPOOL_SIZE}"
echo "TX_BURST:      ${TX_BURST}"
echo "RX_BURST:      ${RX_BURST}"
echo "TICK_US:       ${TICK_US}"
echo "FC_MODE:       ${FC_MODE}"
echo "INITIAL_FCCL:  ${INITIAL_FCCL}"
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
"${SENDER_BIN}" "${EAL_OPTS[@]}" -- \
  --csv "${CSV_FILE}" \
  --port "${PORT_ID}" \
  --vcs "${NB_VC}" \
  --ring-size "${RING_SIZE}" \
  --pkt-size "${PKT_SIZE}" \
  --mempool "${MEMPOOL_SIZE}" \
  --tx-burst "${TX_BURST}" \
  --rx-burst "${RX_BURST}" \
  --tick-us "${TICK_US}" \
  --fc-mode "${FC_MODE}" \
  --initial-fccl "${INITIAL_FCCL}"
EXIT_CODE=$?
set -e
set +x

if [[ ${EXIT_CODE} -ne 0 ]]; then
  echo
  echo "[run_sender] exited with code: ${EXIT_CODE}"
else
  echo
  echo "[run_sender] exited normally"
fi

exit "${EXIT_CODE}"
