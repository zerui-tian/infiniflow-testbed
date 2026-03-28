#!/usr/bin/env bash
set -euo pipefail

#
# 启动 switch 程序
#
# 直接运行:
#   ./scripts/server3_switch.sh
# 可通过修改下方默认参数或导出同名环境变量覆盖。
#

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
SWITCH_BIN="${BUILD_DIR}/switch"

# 默认参数（可直接运行）
DEFAULT_EAL_LCORES="1-16"
DEFAULT_EAL_MEM_CHANNELS="4"
DEFAULT_EAL_PCI_ADDRS="0000:c1:00.1,0000:81:00.0"
DEFAULT_LOG_LEVEL="INFO"
DEFAULT_INGRESS_PORTS="1"
DEFAULT_EGRESS_PORT="0"
DEFAULT_NB_VC="8"
DEFAULT_VC_RING_SIZE="1024"
DEFAULT_FEEDBACK_RING_SIZE="1024"
DEFAULT_PKT_SIZE="1500"
DEFAULT_MEMPOOL_SIZE="32768"
DEFAULT_TX_BURST="64"
DEFAULT_RX_BURST="64"
DEFAULT_FC_MODE="cbfc"
DEFAULT_INITIAL_FCCL="1024"
DEFAULT_VC_CAPACITY="1024"

EAL_LCORES="${EAL_LCORES:-$DEFAULT_EAL_LCORES}"
EAL_MEM_CHANNELS="${EAL_MEM_CHANNELS:-$DEFAULT_EAL_MEM_CHANNELS}"
EAL_PCI_ADDRS="${EAL_PCI_ADDRS:-$DEFAULT_EAL_PCI_ADDRS}"
LOG_LEVEL="${LOG_LEVEL:-$DEFAULT_LOG_LEVEL}"
INGRESS_PORTS="${INGRESS_PORTS:-$DEFAULT_INGRESS_PORTS}"
EGRESS_PORT="${EGRESS_PORT:-$DEFAULT_EGRESS_PORT}"
NB_VC="${NB_VC:-$DEFAULT_NB_VC}"
VC_RING_SIZE="${VC_RING_SIZE:-$DEFAULT_VC_RING_SIZE}"
FEEDBACK_RING_SIZE="${FEEDBACK_RING_SIZE:-$DEFAULT_FEEDBACK_RING_SIZE}"
PKT_SIZE="${PKT_SIZE:-$DEFAULT_PKT_SIZE}"
MEMPOOL_SIZE="${MEMPOOL_SIZE:-$DEFAULT_MEMPOOL_SIZE}"
TX_BURST="${TX_BURST:-$DEFAULT_TX_BURST}"
RX_BURST="${RX_BURST:-$DEFAULT_RX_BURST}"
FC_MODE="${FC_MODE:-$DEFAULT_FC_MODE}"
INITIAL_FCCL="${INITIAL_FCCL:-$DEFAULT_INITIAL_FCCL}"
VC_CAPACITY="${VC_CAPACITY:-$DEFAULT_VC_CAPACITY}"

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
      echo "[run_switch] invalid LOG_LEVEL: ${1} (use EMERG|ALERT|CRIT|ERROR|WARN|NOTICE|INFO|DEBUG)" >&2
      exit 1
      ;;
  esac
}

DPDK_LOG_LEVEL="$(to_dpdk_log_level "${LOG_LEVEL}")"

if [[ ! -x "${SWITCH_BIN}" ]]; then
  echo "[run_switch] switch binary not found, building first..."
  cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
  cmake --build "${BUILD_DIR}" -j
fi

echo "========================================"
echo "Start switch"
echo "========================================"
echo "EAL_LCORES:         ${EAL_LCORES}"
echo "EAL_MEM_CH:         ${EAL_MEM_CHANNELS}"
echo "EAL_PCI_ADDRS:      ${EAL_PCI_ADDRS:-<empty>}"
echo "LOG_LEVEL:          ${LOG_LEVEL} (${DPDK_LOG_LEVEL})"
echo "INGRESS_PORTS:      ${INGRESS_PORTS}"
echo "EGRESS_PORT:        ${EGRESS_PORT}"
echo "NB_VC:              ${NB_VC}"
echo "VC_RING_SIZE:       ${VC_RING_SIZE}"
echo "FEEDBACK_RING_SIZE: ${FEEDBACK_RING_SIZE}"
echo "PKT_SIZE:           ${PKT_SIZE}"
echo "MEMPOOL_SIZE:       ${MEMPOOL_SIZE}"
echo "TX_BURST:           ${TX_BURST}"
echo "RX_BURST:           ${RX_BURST}"
echo "FC_MODE:            ${FC_MODE}"
echo "INITIAL_FCCL:       ${INITIAL_FCCL}"
echo "VC_CAPACITY:        ${VC_CAPACITY}"
echo "========================================"
echo

EAL_OPTS=(
  -l "${EAL_LCORES}"
  -n "${EAL_MEM_CHANNELS}"
  --log-level "${DPDK_LOG_LEVEL}"
)

if [[ -n "${EAL_PCI_ADDRS}" ]]; then
  IFS=',' read -r -a PCI_ADDR_ARR <<< "${EAL_PCI_ADDRS}"
  for pci_addr in "${PCI_ADDR_ARR[@]}"; do
    if [[ -n "${pci_addr}" ]]; then
      EAL_OPTS+=(-a "${pci_addr}")
    fi
  done
fi

APP_OPTS=(
  --ingress-ports "${INGRESS_PORTS}"
  --egress-port "${EGRESS_PORT}"
  --vcs "${NB_VC}"
  --vc-ring-size "${VC_RING_SIZE}"
  --feedback-ring-size "${FEEDBACK_RING_SIZE}"
  --pkt-size "${PKT_SIZE}"
  --mempool "${MEMPOOL_SIZE}"
  --tx-burst "${TX_BURST}"
  --rx-burst "${RX_BURST}"
  --fc-mode "${FC_MODE}"
  --initial-fccl "${INITIAL_FCCL}"
  --vc-capacity "${VC_CAPACITY}"
)

set -x
set +e
"${SWITCH_BIN}" "${EAL_OPTS[@]}" -- "${APP_OPTS[@]}"
EXIT_CODE=$?
set -e
set +x

if [[ ${EXIT_CODE} -ne 0 ]]; then
  echo
  echo "[run_switch] exited with code: ${EXIT_CODE}"
else
  echo
  echo "[run_switch] exited normally"
fi

exit "${EXIT_CODE}"
