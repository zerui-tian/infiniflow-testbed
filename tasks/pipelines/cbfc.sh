#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=/dev/null
source "${ROOT_DIR}/tasks/common/lib.sh"

require_cmd ssh

PROFILE_INPUT="${1:-tasks/profiles/cbfc_default.env}"
PROFILE_PATH="$(resolve_profile_path "${PROFILE_INPUT}")"
if [[ ! -f "${PROFILE_PATH}" ]]; then
  log_error "profile not found: ${PROFILE_PATH}"
  exit 1
fi

# shellcheck source=/dev/null
source "${PROFILE_PATH}"

HOST_SWITCH="${HOST_SWITCH:-${ORCH_HOST_SWITCH:-B06-4_server4_ns3_server}}"
HOST_RECEIVER="${HOST_RECEIVER:-${ORCH_HOST_RECEIVER:-B03_tmp2_server2}}"
HOST_SENDER="${HOST_SENDER:-${ORCH_HOST_SENDER:-B03_tmp3_server1}}"
REMOTE_REPO_DIR="${REMOTE_REPO_DIR:-${ORCH_REMOTE_REPO_DIR:-~/infiniflow-testbed}}"
SWITCH_WARMUP_SEC="${SWITCH_WARMUP_SEC:-${ORCH_SWITCH_WARMUP_SEC:-2}}"
RECEIVER_WARMUP_SEC="${RECEIVER_WARMUP_SEC:-${ORCH_RECEIVER_WARMUP_SEC:-2}}"
RUN_SYNC="${RUN_SYNC:-${ORCH_RUN_SYNC:-1}}"
RUN_ID="${TASK_RUN_ID:-$(date +%Y%m%d_%H%M%S)}"
STATE_DIR_REL="${TASK_STATE_DIR:-tasks/state}"
STATE_DIR="${ROOT_DIR}/${STATE_DIR_REL}"

PROFILE_REL="${PROFILE_INPUT}"
if [[ "${PROFILE_INPUT}" = /* ]]; then
  case "${PROFILE_INPUT}" in
    "${ROOT_DIR}"/*)
      PROFILE_REL="${PROFILE_INPUT#${ROOT_DIR}/}"
      ;;
    *)
      log_error "absolute profile must be under repo root: ${ROOT_DIR}"
      exit 1
      ;;
  esac
fi

log_info "run_id=${RUN_ID}"
log_info "profile=${PROFILE_PATH}"
log_info "order: switch -> receiver -> sender"

if [[ "${RUN_SYNC}" == "1" ]]; then
  log_info "syncing project to remote hosts..."
  bash "${ROOT_DIR}/sync.sh"
fi

switch_log="output/logs/${RUN_ID}_switch.log"
receiver_log="output/logs/${RUN_ID}_receiver.log"
sender_log="output/logs/${RUN_ID}_sender.log"

log_info "starting switch on ${HOST_SWITCH}..."
switch_pid="$(start_remote_background \
  "${HOST_SWITCH}" "${REMOTE_REPO_DIR}" \
  "tasks/hosts/server4/switch.sh" "${PROFILE_REL}" "${RUN_ID}" "${switch_log}")"
if ! check_remote_pid "${HOST_SWITCH}" "${REMOTE_REPO_DIR}" "${switch_pid}"; then
  log_error "switch failed to stay alive on ${HOST_SWITCH} (pid=${switch_pid})"
  exit 1
fi
log_info "switch started: host=${HOST_SWITCH} pid=${switch_pid} log=${switch_log}"

sleep "${SWITCH_WARMUP_SEC}"

log_info "starting receiver on ${HOST_RECEIVER}..."
receiver_pid="$(start_remote_background \
  "${HOST_RECEIVER}" "${REMOTE_REPO_DIR}" \
  "tasks/hosts/server2/receiver.sh" "${PROFILE_REL}" "${RUN_ID}" "${receiver_log}")"
if ! check_remote_pid "${HOST_RECEIVER}" "${REMOTE_REPO_DIR}" "${receiver_pid}"; then
  log_error "receiver failed to stay alive on ${HOST_RECEIVER} (pid=${receiver_pid})"
  exit 1
fi
log_info "receiver started: host=${HOST_RECEIVER} pid=${receiver_pid} log=${receiver_log}"

sleep "${RECEIVER_WARMUP_SEC}"

log_info "starting sender on ${HOST_SENDER}..."
sender_pid="$(start_remote_background \
  "${HOST_SENDER}" "${REMOTE_REPO_DIR}" \
  "tasks/hosts/server3/sender.sh" "${PROFILE_REL}" "${RUN_ID}" "${sender_log}")"
if ! check_remote_pid "${HOST_SENDER}" "${REMOTE_REPO_DIR}" "${sender_pid}"; then
  log_error "sender failed to stay alive on ${HOST_SENDER} (pid=${sender_pid})"
  exit 1
fi
log_info "sender started: host=${HOST_SENDER} pid=${sender_pid} log=${sender_log}"

mkdir -p "${STATE_DIR}"
STATE_FILE="${STATE_DIR}/${RUN_ID}.env"
LATEST_FILE="${STATE_DIR}/latest.env"
cat > "${STATE_FILE}" <<EOF
RUN_ID="${RUN_ID}"
PROFILE_REL="${PROFILE_REL}"
REMOTE_REPO_DIR="${REMOTE_REPO_DIR}"
HOST_SWITCH="${HOST_SWITCH}"
HOST_RECEIVER="${HOST_RECEIVER}"
HOST_SENDER="${HOST_SENDER}"
SWITCH_PID="${switch_pid}"
RECEIVER_PID="${receiver_pid}"
SENDER_PID="${sender_pid}"
SWITCH_LOG="${switch_log}"
RECEIVER_LOG="${receiver_log}"
SENDER_LOG="${sender_log}"
EOF
cp "${STATE_FILE}" "${LATEST_FILE}"

echo
log_info "all components started successfully"
echo "  switch : ${HOST_SWITCH} pid=${switch_pid} log=${switch_log}"
echo "  receiver: ${HOST_RECEIVER} pid=${receiver_pid} log=${receiver_log}"
echo "  sender : ${HOST_SENDER} pid=${sender_pid} log=${sender_log}"
echo "  state  : ${STATE_FILE}"
