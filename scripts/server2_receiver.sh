#!/usr/bin/env bash
set -euo pipefail

#
# 启动 receiver 程序
#
# 直接运行:
#   ./scripts/run_receiver.sh
# 可通过修改下方默认参数或导出同名环境变量覆盖。
#

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
RECEIVER_BIN="${BUILD_DIR}/receiver"

# 默认参数（可直接运行）
DEFAULT_EAL_LCORES="5"
DEFAULT_EAL_MEM_CHANNELS="4"
DEFAULT_EAL_PCI_ADDR="0000:01:00.1"
DEFAULT_PORT_ID="0"
DEFAULT_RX_BURST="64"
DEFAULT_PKT_SIZE="8000"
DEFAULT_MEMPOOL_SIZE="32768"
DEFAULT_OUTPUT_FILE="${ROOT_DIR}/receiver_flow_stats.csv"

EAL_LCORES="${EAL_LCORES:-$DEFAULT_EAL_LCORES}"
EAL_MEM_CHANNELS="${EAL_MEM_CHANNELS:-$DEFAULT_EAL_MEM_CHANNELS}"
EAL_PCI_ADDR="${EAL_PCI_ADDR:-$DEFAULT_EAL_PCI_ADDR}"
PORT_ID="${PORT_ID:-$DEFAULT_PORT_ID}"
RX_BURST="${RX_BURST:-$DEFAULT_RX_BURST}"
PKT_SIZE="${PKT_SIZE:-$DEFAULT_PKT_SIZE}"
MEMPOOL_SIZE="${MEMPOOL_SIZE:-$DEFAULT_MEMPOOL_SIZE}"
OUTPUT_FILE="${OUTPUT_FILE:-$DEFAULT_OUTPUT_FILE}"

if [[ ! -x "${RECEIVER_BIN}" ]]; then
  echo "[run_receiver] receiver binary not found, building first..."
  cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
  cmake --build "${BUILD_DIR}" -j
fi

echo "========================================"
echo "Start receiver"
echo "========================================"
echo "EAL_LCORES:    ${EAL_LCORES}"
echo "EAL_MEM_CH:    ${EAL_MEM_CHANNELS}"
echo "EAL_PCI_ADDR:  ${EAL_PCI_ADDR:-<empty>}"
echo "PORT_ID:       ${PORT_ID}"
echo "RX_BURST:      ${RX_BURST}"
echo "PKT_SIZE:      ${PKT_SIZE}"
echo "MEMPOOL_SIZE:  ${MEMPOOL_SIZE}"
echo "OUTPUT_FILE:   ${OUTPUT_FILE}"
echo "========================================"
echo

EAL_OPTS=(
  -l "${EAL_LCORES}"
  -n "${EAL_MEM_CHANNELS}"
)
if [[ -n "${EAL_PCI_ADDR}" ]]; then
  EAL_OPTS+=(-a "${EAL_PCI_ADDR}")
fi


set -x
set +e
"${RECEIVER_BIN}" "${EAL_OPTS[@]}" -- \
  --port "${PORT_ID}" \
  --rx-burst "${RX_BURST}" \
  --pkt-size "${PKT_SIZE}" \
  --mempool "${MEMPOOL_SIZE}" \
  --output "${OUTPUT_FILE}"
EXIT_CODE=$?
set -e
set +x

if [[ ${EXIT_CODE} -ne 0 ]]; then
  echo
  echo "[run_receiver] exited with code: ${EXIT_CODE}"
else
  echo
  echo "[run_receiver] exited normally"
fi

exit "${EXIT_CODE}"
