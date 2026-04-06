#!/usr/bin/env bash
set -euo pipefail

#
# 启动 receiver 程序
#
# 直接运行:
#   ./scripts/run_receiver.sh
# 可通过修改下方默认参数或导出同名环境变量覆盖。
#

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
RECEIVER_BIN="${BUILD_DIR}/receiver"

# 默认参数（可直接运行）
DEFAULT_EAL_LCORES="0-8"
DEFAULT_EAL_MEM_CHANNELS="4"
DEFAULT_EAL_PCI_ADDRS="0000:01:00.0,0000:01:00.1"
DEFAULT_LOG_LEVEL="INFO"
DEFAULT_PORTS="0,1"
DEFAULT_NB_VC="4"
DEFAULT_RX_BURST="64"
DEFAULT_TX_BURST="64"
DEFAULT_PKT_SIZE="9000"
DEFAULT_MEMPOOL_SIZE="32768"
# DEFAULT_FC_MODE="none"
DEFAULT_FC_MODE="cbfc"
DEFAULT_CBFC_BUFFER_PKTS="80"
DEFAULT_FEEDBACK_RING_SIZE="1024"
DEFAULT_OUTPUT_FILE="${ROOT_DIR}/output/cbfc_test.csv"

EAL_LCORES="${EAL_LCORES:-$DEFAULT_EAL_LCORES}"
EAL_MEM_CHANNELS="${EAL_MEM_CHANNELS:-$DEFAULT_EAL_MEM_CHANNELS}"
EAL_PCI_ADDRS="${EAL_PCI_ADDRS:-${EAL_PCI_ADDR:-$DEFAULT_EAL_PCI_ADDRS}}"
LOG_LEVEL="${LOG_LEVEL:-$DEFAULT_LOG_LEVEL}"
PORTS="${PORTS:-$DEFAULT_PORTS}"
NB_VC="${NB_VC:-$DEFAULT_NB_VC}"
RX_BURST="${RX_BURST:-$DEFAULT_RX_BURST}"
TX_BURST="${TX_BURST:-$DEFAULT_TX_BURST}"
PKT_SIZE="${PKT_SIZE:-$DEFAULT_PKT_SIZE}"
MEMPOOL_SIZE="${MEMPOOL_SIZE:-$DEFAULT_MEMPOOL_SIZE}"
FC_MODE="${FC_MODE:-$DEFAULT_FC_MODE}"
CBFC_BUFFER_PKTS="${CBFC_BUFFER_PKTS:-$DEFAULT_CBFC_BUFFER_PKTS}"
FEEDBACK_RING_SIZE="${FEEDBACK_RING_SIZE:-$DEFAULT_FEEDBACK_RING_SIZE}"
OUTPUT_FILE="${OUTPUT_FILE:-$DEFAULT_OUTPUT_FILE}"

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
      echo "[receiver] invalid LOG_LEVEL: ${1} (use EMERG|ALERT|CRIT|ERROR|WARN|NOTICE|INFO|DEBUG)" >&2
      exit 1
      ;;
  esac
}

DPDK_LOG_LEVEL="$(to_dpdk_log_level "${LOG_LEVEL}")"

if [[ ! -x "${RECEIVER_BIN}" ]]; then
  echo "[receiver_server2] receiver binary not found, building first..."
  cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
  cmake --build "${BUILD_DIR}" -j
fi

split_csv() {
  local input="$1"
  local -n out_arr="$2"
  out_arr=()
  IFS=',' read -r -a out_arr <<< "${input}"
}

trim() {
  local s="$1"
  s="${s#"${s%%[![:space:]]*}"}"
  s="${s%"${s##*[![:space:]]}"}"
  printf '%s' "${s}"
}

split_csv "${EAL_PCI_ADDRS}" PCI_ARR

for i in "${!PCI_ARR[@]}"; do
  PCI_ARR[$i]="$(trim "${PCI_ARR[$i]}")"
done

echo "========================================"
echo "Start receiver"
echo "========================================"
echo "EAL_LCORES:    ${EAL_LCORES}"
echo "EAL_MEM_CH:    ${EAL_MEM_CHANNELS}"
echo "EAL_PCI_ADDRS: ${EAL_PCI_ADDRS:-<empty>}"
echo "LOG_LEVEL:     ${LOG_LEVEL} (${DPDK_LOG_LEVEL})"
echo "PORTS:         ${PORTS}"
echo "NB_VC:         ${NB_VC}"
echo "RX_BURST:      ${RX_BURST}"
echo "TX_BURST:      ${TX_BURST}"
echo "PKT_SIZE:      ${PKT_SIZE}"
echo "MEMPOOL_SIZE:  ${MEMPOOL_SIZE}"
echo "FC_MODE:       ${FC_MODE}"
echo "CBFC_BUFFER:   ${CBFC_BUFFER_PKTS}"
echo "FB_RING_SIZE:  ${FEEDBACK_RING_SIZE}"
echo "OUTPUT_FILE:   ${OUTPUT_FILE}"
echo "========================================"
echo

EAL_OPTS=(
  -l "${EAL_LCORES}"
  -n "${EAL_MEM_CHANNELS}"
  --log-level "${DPDK_LOG_LEVEL}"
)
for pci in "${PCI_ARR[@]}"; do
  if [[ -n "${pci}" ]]; then
    EAL_OPTS+=(-a "${pci}")
  fi
done


set -x
set +e
"${RECEIVER_BIN}" "${EAL_OPTS[@]}" -- \
  --ports "${PORTS}" \
  --vcs "${NB_VC}" \
  --rx-burst "${RX_BURST}" \
  --tx-burst "${TX_BURST}" \
  --pkt-size "${PKT_SIZE}" \
  --mempool "${MEMPOOL_SIZE}" \
  --fc-mode "${FC_MODE}" \
  --cbfc-buffer-pkts "${CBFC_BUFFER_PKTS}" \
  --feedback-ring-size "${FEEDBACK_RING_SIZE}" \
  --output "${OUTPUT_FILE}"
EXIT_CODE=$?
set -e
set +x

if [[ ${EXIT_CODE} -ne 0 ]]; then
  echo
  echo "[receiver] exited with code: ${EXIT_CODE}"
else
  echo
  echo "[receiver] exited normally"
fi

exit "${EXIT_CODE}"
