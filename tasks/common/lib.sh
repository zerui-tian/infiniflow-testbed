#!/usr/bin/env bash
set -euo pipefail

TASKS_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${TASKS_LIB_DIR}/../.." && pwd)"

log_info() {
  echo "[tasks] $*"
}

log_error() {
  echo "[tasks] ERROR: $*" >&2
}

require_cmd() {
  local cmd="$1"
  if ! command -v "${cmd}" >/dev/null 2>&1; then
    log_error "required command not found: ${cmd}"
    exit 1
  fi
}

resolve_profile_path() {
  local input_path="${1:-tasks/profiles/cbfc_default.env}"
  if [[ "${input_path}" = /* ]]; then
    echo "${input_path}"
  else
    echo "${ROOT_DIR}/${input_path}"
  fi
}

resolve_state_path() {
  local input_path="${1:-tasks/state/latest.env}"
  if [[ "${input_path}" = /* ]]; then
    echo "${input_path}"
  else
    echo "${ROOT_DIR}/${input_path}"
  fi
}

start_remote_background() {
  local host="$1"
  local remote_repo_dir="$2"
  local role_script="$3"
  local profile_rel="$4"
  local run_id="$5"
  local remote_log_path="$6"
  local remote_pid

  remote_pid="$(
    ssh "${host}" bash -s -- \
      "${remote_repo_dir}" \
      "${role_script}" \
      "${profile_rel}" \
      "${run_id}" \
      "${remote_log_path}" <<'EOF'
set -euo pipefail
repo_dir="$1"
role_script="$2"
profile_rel="$3"
run_id="$4"
log_path="$5"

if [[ "${repo_dir}" == "~"* ]]; then
  repo_dir="${HOME}${repo_dir#"~"}"
fi

cd "${repo_dir}"
mkdir -p "$(dirname "${log_path}")"

TASK_PROFILE="${profile_rel}" TASK_RUN_ID="${run_id}" nohup bash "${role_script}" \
  > "${log_path}" 2>&1 < /dev/null &
echo "$!"
EOF
  )"

  # Strip whitespace/newlines from ssh output.
  remote_pid="${remote_pid//$'\r'/}"
  remote_pid="${remote_pid//$'\n'/}"
  echo "${remote_pid}"
}

check_remote_pid() {
  local host="$1"
  local remote_repo_dir="$2"
  local pid="$3"
  ssh "${host}" bash -s -- "${remote_repo_dir}" "${pid}" <<'EOF'
set -euo pipefail
repo_dir="$1"
pid="$2"
if [[ "${repo_dir}" == "~"* ]]; then
  repo_dir="${HOME}${repo_dir#"~"}"
fi
cd "${repo_dir}"
kill -0 "${pid}" >/dev/null 2>&1
EOF
}

signal_remote_pid() {
  local host="$1"
  local remote_repo_dir="$2"
  local pid="$3"
  local signal_name="${4:-TERM}"
  ssh "${host}" bash -s -- "${remote_repo_dir}" "${pid}" "${signal_name}" <<'EOF'
set -euo pipefail
repo_dir="$1"
pid="$2"
signal_name="$3"
if [[ "${repo_dir}" == "~"* ]]; then
  repo_dir="${HOME}${repo_dir#"~"}"
fi
cd "${repo_dir}"
if kill -0 "${pid}" >/dev/null 2>&1; then
  kill "-${signal_name}" "${pid}"
  echo "signaled"
else
  echo "not_running"
fi
EOF
}

pid_status_remote() {
  local host="$1"
  local remote_repo_dir="$2"
  local pid="$3"
  ssh "${host}" bash -s -- "${remote_repo_dir}" "${pid}" <<'EOF'
set -euo pipefail
repo_dir="$1"
pid="$2"
if [[ "${repo_dir}" == "~"* ]]; then
  repo_dir="${HOME}${repo_dir#"~"}"
fi
cd "${repo_dir}"
if kill -0 "${pid}" >/dev/null 2>&1; then
  echo "running"
else
  echo "stopped"
fi
EOF
}
