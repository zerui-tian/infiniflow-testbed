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

id_to_token() {
  local raw="$1"
  echo "${raw}" | tr '[:lower:]-' '[:upper:]_' | tr -c 'A-Z0-9_' '_'
}

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

if [[ -n "${RECEIVER_IDS:-}" ]]; then
  for receiver_id in ${RECEIVER_IDS}; do
    receiver_tok="$(id_to_token "${receiver_id}")"
    receiver_host_var="RECEIVER_${receiver_tok}_HOST"
    receiver_pid_var="RECEIVER_${receiver_tok}_PID"
    receiver_log_var="RECEIVER_${receiver_tok}_LOG"
    print_one "receiver(${receiver_id})" \
      "${!receiver_host_var}" \
      "${!receiver_pid_var}" \
      "${!receiver_log_var}"
  done
elif [[ -n "${HOST_RECEIVER:-}" && -n "${RECEIVER_PID:-}" ]]; then
  print_one "receiver" "${HOST_RECEIVER}" "${RECEIVER_PID}" "${RECEIVER_LOG:-}"
fi

if [[ -n "${SENDER_IDS:-}" ]]; then
  for sender_id in ${SENDER_IDS}; do
    sender_tok="$(id_to_token "${sender_id}")"
    sender_host_var="SENDER_${sender_tok}_HOST"
    sender_pid_var="SENDER_${sender_tok}_PID"
    sender_log_var="SENDER_${sender_tok}_LOG"
    print_one "sender(${sender_id})" \
      "${!sender_host_var}" \
      "${!sender_pid_var}" \
      "${!sender_log_var}"
  done
elif [[ -n "${HOST_SENDER:-}" && -n "${SENDER_PID:-}" ]]; then
  print_one "sender" "${HOST_SENDER}" "${SENDER_PID}" "${SENDER_LOG:-}"
fi
