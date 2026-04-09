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

STOP_GRACE_SEC="${STOP_GRACE_SEC:-3}"

id_to_token() {
  local raw="$1"
  echo "${raw}" | tr '[:lower:]-' '[:upper:]_' | tr -c 'A-Z0-9_' '_'
}

stop_one() {
  local name="$1"
  local host="$2"
  local pid="$3"
  local status_out

  status_out="$(pid_status_remote "${host}" "${REMOTE_REPO_DIR}" "${pid}")"
  if [[ "${status_out}" != "running" ]]; then
    log_info "${name} already stopped: host=${host} pid=${pid}"
    return 0
  fi

  log_info "stopping ${name}: host=${host} pid=${pid} (SIGTERM)"
  signal_remote_pid "${host}" "${REMOTE_REPO_DIR}" "${pid}" TERM >/dev/null

  local i
  for ((i = 0; i < STOP_GRACE_SEC; i++)); do
    sleep 1
    status_out="$(pid_status_remote "${host}" "${REMOTE_REPO_DIR}" "${pid}")"
    if [[ "${status_out}" != "running" ]]; then
      log_info "${name} stopped gracefully"
      return 0
    fi
  done

  log_info "${name} still running, sending SIGKILL: host=${host} pid=${pid}"
  signal_remote_pid "${host}" "${REMOTE_REPO_DIR}" "${pid}" KILL >/dev/null || true
  sleep 1
  status_out="$(pid_status_remote "${host}" "${REMOTE_REPO_DIR}" "${pid}")"
  if [[ "${status_out}" == "running" ]]; then
    log_error "failed to stop ${name}: host=${host} pid=${pid}"
    return 1
  fi
  log_info "${name} stopped after SIGKILL"
}

log_info "run_id=${RUN_ID:-unknown}"
log_info "state=${STATE_PATH}"
log_info "stop order: senders -> receivers -> switch"

if [[ -n "${SENDER_IDS:-}" ]]; then
  for sender_id in ${SENDER_IDS}; do
    sender_tok="$(id_to_token "${sender_id}")"
    sender_host_var="SENDER_${sender_tok}_HOST"
    sender_pid_var="SENDER_${sender_tok}_PID"
    stop_one "sender(${sender_id})" "${!sender_host_var}" "${!sender_pid_var}"
  done
elif [[ -n "${HOST_SENDER:-}" && -n "${SENDER_PID:-}" ]]; then
  stop_one "sender" "${HOST_SENDER}" "${SENDER_PID}"
fi

if [[ -n "${RECEIVER_IDS:-}" ]]; then
  for receiver_id in ${RECEIVER_IDS}; do
    receiver_tok="$(id_to_token "${receiver_id}")"
    receiver_host_var="RECEIVER_${receiver_tok}_HOST"
    receiver_pid_var="RECEIVER_${receiver_tok}_PID"
    stop_one "receiver(${receiver_id})" "${!receiver_host_var}" "${!receiver_pid_var}"
  done
elif [[ -n "${HOST_RECEIVER:-}" && -n "${RECEIVER_PID:-}" ]]; then
  stop_one "receiver" "${HOST_RECEIVER}" "${RECEIVER_PID}"
fi

stop_one "switch" "${HOST_SWITCH}" "${SWITCH_PID}"

log_info "all components stopped"
