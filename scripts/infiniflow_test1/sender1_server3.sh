#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
SENDER_BIN="${BUILD_DIR}/sender"

DEFAULT_EAL_LCORES="1-16"
DEFAULT_EAL_MEM_CHANNELS="8"
DEFAULT_EAL_PCI_ADDRS="0000:41:00.0"
DEFAULT_LOG_LEVEL="INFO"
DEFAULT_CSV_FILES="${ROOT_DIR}/examples/infiniflow_test1/sender1.csv"
DEFAULT_PORT_IDS="0"
DEFAULT_NB_VC="8"
DEFAULT_RING_SIZE="8192"
DEFAULT_PKT_SIZE="9000"
DEFAULT_MEMPOOL_SIZE="32768"
DEFAULT_TX_BURST="4"
DEFAULT_RX_BURST="64"
DEFAULT_TICK_US="1"
DEFAULT_FC_MODE="infiniflow"
DEFAULT_INITIAL_FCCL="8"
DEFAULT_QMIN="2"
DEFAULT_QMAX="4"
DEFAULT_INITIAL_THRESHOLD="8"

EAL_LCORES="${EAL_LCORES:-$DEFAULT_EAL_LCORES}"
EAL_MEM_CHANNELS="${EAL_MEM_CHANNELS:-$DEFAULT_EAL_MEM_CHANNELS}"
EAL_PCI_ADDRS="${EAL_PCI_ADDRS:-${EAL_PCI_ADDR:-$DEFAULT_EAL_PCI_ADDRS}}"
LOG_LEVEL="${LOG_LEVEL:-$DEFAULT_LOG_LEVEL}"
CSV_FILES="${CSV_FILES:-${CSV_FILE:-$DEFAULT_CSV_FILES}}"
PORT_IDS="${PORT_IDS:-${PORT_ID:-$DEFAULT_PORT_IDS}}"
NB_VC="${NB_VC:-$DEFAULT_NB_VC}"
RING_SIZE="${RING_SIZE:-$DEFAULT_RING_SIZE}"
PKT_SIZE="${PKT_SIZE:-$DEFAULT_PKT_SIZE}"
MEMPOOL_SIZE="${MEMPOOL_SIZE:-$DEFAULT_MEMPOOL_SIZE}"
TX_BURST="${TX_BURST:-$DEFAULT_TX_BURST}"
RX_BURST="${RX_BURST:-$DEFAULT_RX_BURST}"
TICK_US="${TICK_US:-$DEFAULT_TICK_US}"
FC_MODE="${FC_MODE:-$DEFAULT_FC_MODE}"
INITIAL_FCCL="${INITIAL_FCCL:-$DEFAULT_INITIAL_FCCL}"
QMIN="${QMIN:-$DEFAULT_QMIN}"
QMAX="${QMAX:-$DEFAULT_QMAX}"
INITIAL_THRESHOLD="${INITIAL_THRESHOLD:-$DEFAULT_INITIAL_THRESHOLD}"

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
      echo "[run_sender] invalid LOG_LEVEL: ${1}" >&2
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

split_csv "${PORT_IDS}" PORT_ARR
split_csv "${CSV_FILES}" CSV_ARR
split_csv "${EAL_PCI_ADDRS}" PCI_ARR

if [[ ${#PORT_ARR[@]} -eq 0 || ${#CSV_ARR[@]} -eq 0 || ${#PORT_ARR[@]} -ne ${#CSV_ARR[@]} ]]; then
  echo "[run_sender] invalid PORT_IDS/CSV_FILES" >&2
  exit 1
fi

for i in "${!PORT_ARR[@]}"; do
  PORT_ARR[$i]="$(trim "${PORT_ARR[$i]}")"
  CSV_ARR[$i]="$(trim "${CSV_ARR[$i]}")"
  if [[ -z "${PORT_ARR[$i]}" || -z "${CSV_ARR[$i]}" || ! -f "${CSV_ARR[$i]}" ]]; then
    echo "[run_sender] invalid port/csv entry at index ${i}" >&2
    exit 1
  fi
done

for i in "${!PCI_ARR[@]}"; do
  PCI_ARR[$i]="$(trim "${PCI_ARR[$i]}")"
done

echo "========================================"
echo "Start sender"
echo "========================================"
echo "FC_MODE:            ${FC_MODE}"
echo "INITIAL_FCCL:       ${INITIAL_FCCL}"
echo "QMIN:               ${QMIN}"
echo "QMAX:               ${QMAX}"
echo "INITIAL_THRESHOLD:  ${INITIAL_THRESHOLD}"
echo "========================================"
echo

EAL_OPTS=(-l "${EAL_LCORES}" -n "${EAL_MEM_CHANNELS}" --log-level "${DPDK_LOG_LEVEL}")
for pci in "${PCI_ARR[@]}"; do
  if [[ -n "${pci}" ]]; then
    EAL_OPTS+=(-a "${pci}")
  fi
done

set -x
set +e
"${SENDER_BIN}" "${EAL_OPTS[@]}" -- \
  --csvs "${CSV_FILES}" \
  --ports "${PORT_IDS}" \
  --vcs "${NB_VC}" \
  --ring-size "${RING_SIZE}" \
  --pkt-size "${PKT_SIZE}" \
  --mempool "${MEMPOOL_SIZE}" \
  --tx-burst "${TX_BURST}" \
  --rx-burst "${RX_BURST}" \
  --tick-us "${TICK_US}" \
  --fc-mode "${FC_MODE}" \
  --initial-fccl "${INITIAL_FCCL}" \
  --qmin "${QMIN}" \
  --qmax "${QMAX}" \
  --initial-threshold "${INITIAL_THRESHOLD}"
EXIT_CODE=$?
set -e
set +x

exit "${EXIT_CODE}"
