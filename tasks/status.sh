#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=/dev/null
source "${ROOT_DIR}/tasks/common/lib.sh"

require_cmd ssh

STATE_INPUT="${1:-tasks/state/latest.env}"
STATE_PATH="$(resolve_state_path "${STATE_INPUT}")"
if [[ ! -f "${STATE_PATH}" ]]; then
  log_error "state file not found: ${STATE_PATH}"
  exit 1
fi

# shellcheck source=/dev/null
source "${STATE_PATH}"

print_one() {
  local name="$1"
  local host="$2"
  local pid="$3"
  local log_path="$4"
  local state

  state="$(pid_status_remote "${host}" "${REMOTE_REPO_DIR}" "${pid}")"
  echo "${name}: ${state} (host=${host} pid=${pid} log=${log_path})"
}

echo "[tasks] run_id=${RUN_ID:-unknown}"
echo "[tasks] profile=${PROFILE_REL:-unknown}"
echo "[tasks] state=${STATE_PATH}"
echo "[tasks] repo=${REMOTE_REPO_DIR}"
print_one "switch" "${HOST_SWITCH}" "${SWITCH_PID}" "${SWITCH_LOG}"
print_one "receiver" "${HOST_RECEIVER}" "${RECEIVER_PID}" "${RECEIVER_LOG}"
print_one "sender" "${HOST_SENDER}" "${SENDER_PID}" "${SENDER_LOG}"
