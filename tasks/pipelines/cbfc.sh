#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=/dev/null
source "${ROOT_DIR}/tasks/common/lib.sh"

require_cmd ssh

id_to_token() {
  local raw="$1"
  echo "${raw}" | tr '[:lower:]-' '[:upper:]_' | tr -c 'A-Z0-9_' '_'
}

trim_spaces() {
  local s="$1"
  s="${s#"${s%%[![:space:]]*}"}"
  s="${s%"${s##*[![:space:]]}"}"
  echo "${s}"
}

parse_csv_to_array() {
  local raw="$1"
  local -n out_ref="$2"
  local part
  IFS=',' read -r -a out_ref <<< "${raw}"
  for i in "${!out_ref[@]}"; do
    part="$(trim_spaces "${out_ref[$i]}")"
    out_ref[$i]="${part}"
  done
}

collect_prefixed_env_kvs() {
  local prefix="$1"
  local -n out_ref="$2"
  local name suffix value
  out_ref=()
  while IFS= read -r name; do
    suffix="${name#${prefix}}"
    case "${suffix}" in
      HOST|SCRIPT|LOG_TAG|WARMUP_SEC)
        continue
        ;;
    esac
    value="${!name}"
    out_ref+=("${suffix}=${value}")
  done < <(compgen -A variable "${prefix}")
}

PROFILE_INPUT="${1:-tasks/profiles/cbfc_default.env}"
PROFILE_PATH="$(resolve_profile_path "${PROFILE_INPUT}")"
if [[ ! -f "${PROFILE_PATH}" ]]; then
  log_error "profile not found: ${PROFILE_PATH}"
  exit 1
fi

# shellcheck source=/dev/null
source "${PROFILE_PATH}"

SWITCH_HOST="${SWITCH_HOST:-${ORCH_SWITCH_HOST:-${ORCH_HOST_SWITCH:-B06-4_server4_ns3_server}}}"
SWITCH_SCRIPT="${SWITCH_SCRIPT:-${ORCH_SWITCH_SCRIPT:-tasks/hosts/server4/switch.sh}}"
SENDERS_CSV="${SENDERS_CSV:-${ORCH_SENDERS:-default}}"
RECEIVERS_CSV="${RECEIVERS_CSV:-${ORCH_RECEIVERS:-default}}"
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
log_info "order: switch -> receivers -> senders"

if [[ "${RUN_SYNC}" == "1" ]]; then
  log_info "syncing project to remote hosts..."
  bash "${ROOT_DIR}/sync.sh"
fi

switch_log="output/logs/${RUN_ID}_switch.log"
declare -a sender_ids=()
declare -a receiver_ids=()
declare -A sender_host_map=()
declare -A sender_pid_map=()
declare -A sender_log_map=()
declare -A receiver_host_map=()
declare -A receiver_pid_map=()
declare -A receiver_log_map=()

parse_csv_to_array "${SENDERS_CSV}" sender_ids
parse_csv_to_array "${RECEIVERS_CSV}" receiver_ids

if [[ ${#sender_ids[@]} -eq 1 && "${sender_ids[0]}" == "default" ]]; then
  if [[ -n "${ORCH_HOST_SENDER:-}" ]]; then
    sender_ids=("sender")
    SENDER_SENDER_HOST="${ORCH_HOST_SENDER}"
  else
    log_error "no sender instance configured (set ORCH_SENDERS)"
    exit 1
  fi
fi

if [[ ${#receiver_ids[@]} -eq 1 && "${receiver_ids[0]}" == "default" ]]; then
  if [[ -n "${ORCH_HOST_RECEIVER:-}" ]]; then
    receiver_ids=("receiver")
    RECEIVER_RECEIVER_HOST="${ORCH_HOST_RECEIVER}"
  else
    log_error "no receiver instance configured (set ORCH_RECEIVERS)"
    exit 1
  fi
fi

log_info "starting switch on ${SWITCH_HOST}..."
switch_pid="$(start_remote_background \
  "${SWITCH_HOST}" "${REMOTE_REPO_DIR}" \
  "${SWITCH_SCRIPT}" "${PROFILE_REL}" "${RUN_ID}" "${switch_log}")"
if ! check_remote_pid "${SWITCH_HOST}" "${REMOTE_REPO_DIR}" "${switch_pid}"; then
  log_error "switch failed to stay alive on ${SWITCH_HOST} (pid=${switch_pid})"
  exit 1
fi
log_info "switch started: host=${SWITCH_HOST} pid=${switch_pid} log=${switch_log}"

sleep "${SWITCH_WARMUP_SEC}"

for receiver_id in "${receiver_ids[@]}"; do
  receiver_tok="$(id_to_token "${receiver_id}")"
  receiver_host_var="RECEIVER_${receiver_tok}_HOST"
  receiver_script_var="RECEIVER_${receiver_tok}_SCRIPT"
  receiver_host="${!receiver_host_var:-}"
  receiver_script="${!receiver_script_var:-${ORCH_RECEIVER_SCRIPT:-tasks/hosts/server2/receiver.sh}}"
  if [[ -z "${receiver_host}" ]]; then
    log_error "receiver host missing for id=${receiver_id} (expected ${receiver_host_var})"
    exit 1
  fi
  receiver_log="output/logs/${RUN_ID}_receiver_${receiver_id}.log"
  collect_prefixed_env_kvs "RECEIVER_${receiver_tok}_" receiver_env_kvs
  log_info "starting receiver id=${receiver_id} on ${receiver_host}..."
  receiver_pid="$(start_remote_background_with_env \
    "${receiver_host}" "${REMOTE_REPO_DIR}" \
    "${receiver_script}" "${PROFILE_REL}" "${RUN_ID}" "${receiver_log}" \
    "${receiver_env_kvs[@]}")"
  if ! check_remote_pid "${receiver_host}" "${REMOTE_REPO_DIR}" "${receiver_pid}"; then
    log_error "receiver failed to stay alive: id=${receiver_id} host=${receiver_host} pid=${receiver_pid}"
    exit 1
  fi
  receiver_host_map["${receiver_id}"]="${receiver_host}"
  receiver_pid_map["${receiver_id}"]="${receiver_pid}"
  receiver_log_map["${receiver_id}"]="${receiver_log}"
  log_info "receiver started: id=${receiver_id} host=${receiver_host} pid=${receiver_pid} log=${receiver_log}"
done

sleep "${RECEIVER_WARMUP_SEC}"

for sender_id in "${sender_ids[@]}"; do
  sender_tok="$(id_to_token "${sender_id}")"
  sender_host_var="SENDER_${sender_tok}_HOST"
  sender_script_var="SENDER_${sender_tok}_SCRIPT"
  sender_host="${!sender_host_var:-}"
  sender_script="${!sender_script_var:-${ORCH_SENDER_SCRIPT:-tasks/hosts/server3/sender.sh}}"
  if [[ -z "${sender_host}" ]]; then
    log_error "sender host missing for id=${sender_id} (expected ${sender_host_var})"
    exit 1
  fi
  sender_log="output/logs/${RUN_ID}_sender_${sender_id}.log"
  collect_prefixed_env_kvs "SENDER_${sender_tok}_" sender_env_kvs
  log_info "starting sender id=${sender_id} on ${sender_host}..."
  sender_pid="$(start_remote_background_with_env \
    "${sender_host}" "${REMOTE_REPO_DIR}" \
    "${sender_script}" "${PROFILE_REL}" "${RUN_ID}" "${sender_log}" \
    "${sender_env_kvs[@]}")"
  if ! check_remote_pid "${sender_host}" "${REMOTE_REPO_DIR}" "${sender_pid}"; then
    log_error "sender failed to stay alive: id=${sender_id} host=${sender_host} pid=${sender_pid}"
    exit 1
  fi
  sender_host_map["${sender_id}"]="${sender_host}"
  sender_pid_map["${sender_id}"]="${sender_pid}"
  sender_log_map["${sender_id}"]="${sender_log}"
  log_info "sender started: id=${sender_id} host=${sender_host} pid=${sender_pid} log=${sender_log}"
done

mkdir -p "${STATE_DIR}"
STATE_FILE="${STATE_DIR}/${RUN_ID}.env"
LATEST_FILE="${STATE_DIR}/latest.env"
cat > "${STATE_FILE}" <<EOF
RUN_ID="${RUN_ID}"
PROFILE_REL="${PROFILE_REL}"
REMOTE_REPO_DIR="${REMOTE_REPO_DIR}"
HOST_SWITCH="${SWITCH_HOST}"
SWITCH_PID="${switch_pid}"
SWITCH_LOG="${switch_log}"
SENDER_IDS="${sender_ids[*]}"
RECEIVER_IDS="${receiver_ids[*]}"
EOF

for sender_id in "${sender_ids[@]}"; do
  sender_tok="$(id_to_token "${sender_id}")"
  cat >> "${STATE_FILE}" <<EOF
SENDER_${sender_tok}_ID="${sender_id}"
SENDER_${sender_tok}_HOST="${sender_host_map[${sender_id}]}"
SENDER_${sender_tok}_PID="${sender_pid_map[${sender_id}]}"
SENDER_${sender_tok}_LOG="${sender_log_map[${sender_id}]}"
EOF
done

for receiver_id in "${receiver_ids[@]}"; do
  receiver_tok="$(id_to_token "${receiver_id}")"
  cat >> "${STATE_FILE}" <<EOF
RECEIVER_${receiver_tok}_ID="${receiver_id}"
RECEIVER_${receiver_tok}_HOST="${receiver_host_map[${receiver_id}]}"
RECEIVER_${receiver_tok}_PID="${receiver_pid_map[${receiver_id}]}"
RECEIVER_${receiver_tok}_LOG="${receiver_log_map[${receiver_id}]}"
EOF
done

cp "${STATE_FILE}" "${LATEST_FILE}"

echo
log_info "all components started successfully"
echo "  switch : ${SWITCH_HOST} pid=${switch_pid} log=${switch_log}"
for receiver_id in "${receiver_ids[@]}"; do
  echo "  receiver(${receiver_id}): ${receiver_host_map[${receiver_id}]} pid=${receiver_pid_map[${receiver_id}]} log=${receiver_log_map[${receiver_id}]}"
done
for sender_id in "${sender_ids[@]}"; do
  echo "  sender(${sender_id}): ${sender_host_map[${sender_id}]} pid=${sender_pid_map[${sender_id}]} log=${sender_log_map[${sender_id}]}"
done
echo "  state  : ${STATE_FILE}"
