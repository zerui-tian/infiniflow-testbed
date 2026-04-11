#!/usr/bin/env bash
set -euo pipefail

#
# 启动 switch 程序
#

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
SWITCH_BIN="${BUILD_DIR}/switch"

DEFAULT_EAL_LCORES="1-16"
DEFAULT_EAL_MEM_CHANNELS="8"
# DPDK port 0 -> 0000:81:00.0 -> server2 0000:01:00.1 (receiver port 1)
# DPDK port 1 -> 0000:81:00.1 -> server2 0000:01:00.0 (receiver port 0)
# DPDK port 2 -> 0000:c1:00.0 -> server4 0000:81:00.1
# DPDK port 3 -> 0000:c1:00.1 -> server3 0000:42:00.0
DEFAULT_EAL_PCI_ADDRS="0000:81:00.0,0000:81:00.1,0000:c1:00.0,0000:c1:00.1"
DEFAULT_LOG_LEVEL="INFO"
DEFAULT_INGRESS_PORTS="2,3"
DEFAULT_EGRESS_PORTS="0,1"
DEFAULT_ROUTE_CSV="${ROOT_DIR}/examples/cbfc/switch_server5_routes.csv"
DEFAULT_NB_VC="8"
DEFAULT_VC_RING_SIZE="2048"
DEFAULT_FEEDBACK_RING_SIZE="1024"
DEFAULT_PKT_SIZE="9000"
DEFAULT_MEMPOOL_SIZE="32768"
DEFAULT_TX_BURST="16"
DEFAULT_RX_BURST="16"
DEFAULT_FC_MODE="cbfc"
DEFAULT_INITIAL_FCCL="10"
DEFAULT_VC_CAPACITY="16"

EAL_LCORES="${EAL_LCORES:-$DEFAULT_EAL_LCORES}"
EAL_MEM_CHANNELS="${EAL_MEM_CHANNELS:-$DEFAULT_EAL_MEM_CHANNELS}"
EAL_PCI_ADDRS="${EAL_PCI_ADDRS:-$DEFAULT_EAL_PCI_ADDRS}"
LOG_LEVEL="${LOG_LEVEL:-$DEFAULT_LOG_LEVEL}"
INGRESS_PORTS="${INGRESS_PORTS:-$DEFAULT_INGRESS_PORTS}"
EGRESS_PORTS="${EGRESS_PORTS:-${EGRESS_PORT:-$DEFAULT_EGRESS_PORTS}}"
ROUTE_CSV="${ROUTE_CSV:-$DEFAULT_ROUTE_CSV}"
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

if [[ ! -f "${ROUTE_CSV}" ]]; then
  echo "[run_switch] ROUTE_CSV not found: ${ROUTE_CSV}" >&2
  exit 1
fi

echo "========================================"
echo "Start switch"
echo "========================================"
echo "EAL_LCORES:         ${EAL_LCORES}"
echo "EAL_MEM_CH:         ${EAL_MEM_CHANNELS}"
echo "EAL_PCI_ADDRS:      ${EAL_PCI_ADDRS:-<empty>}"
echo "LOG_LEVEL:          ${LOG_LEVEL} (${DPDK_LOG_LEVEL})"
echo "INGRESS_PORTS:      ${INGRESS_PORTS}"
echo "EGRESS_PORTS:       ${EGRESS_PORTS}"
echo "ROUTE_CSV:          ${ROUTE_CSV}"
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
  --egress-ports "${EGRESS_PORTS}"
  --route-csv "${ROUTE_CSV}"
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
