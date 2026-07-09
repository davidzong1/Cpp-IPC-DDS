#!/usr/bin/env bash
set -euo pipefail

SERVICE_NAME="dzipc_dispatch"
UNIT_NAME="${SERVICE_NAME}.service"
UNIT_FILE="/etc/systemd/system/${UNIT_NAME}"
GROUP_NAME="dzipc_realtime"
LIMITS_FILE="/etc/security/limits.d/${SERVICE_NAME}.conf"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

EXEC_PATH="${DZIPC_DISPATCH_BIN:-}"
DEFAULT_USER="${SUDO_USER:-}"
if [[ -z "${DEFAULT_USER}" ]]; then
    DEFAULT_USER="${USER:-}"
fi
if [[ -z "${DEFAULT_USER}" ]]; then
    DEFAULT_USER="$(id -un 2>/dev/null || true)"
fi
RUN_USER="${DZIPC_DISPATCH_USER:-${DEFAULT_USER}}"
WORK_DIR="${DZIPC_DISPATCH_WORKDIR:-${PROJECT_DIR}}"
MODE="${DZIPC_DISPATCH_MODE:-user}"
RTPRIO_LIMIT="${DZIPC_DISPATCH_RTPRIO:-60}"
ENABLE_SERVICE=1
START_SERVICE=0

usage() {
    cat <<EOF
Usage: $0 [options]

Options:
  --user-mode       configure realtime permission for all dzipc programs run by USER. Default.
  --service         install dzipc_dispatch.service for one executable.
  --exec PATH       dzipc_dispatch executable path. Required in --service mode if auto-detect fails.
  --user USER       target user. Default: current non-root user.
  --workdir DIR     service working directory. Service mode only.
  --rtprio N        realtime priority limit for user mode/systemd service. Default: 60.
  --no-enable       install unit but do not enable it. Service mode only.
  --start           start service after installation. Service mode only.
  -h, --help        show this help.

Environment:
  DZIPC_DISPATCH_MODE=user|service
  DZIPC_DISPATCH_BIN
  DZIPC_DISPATCH_USER
  DZIPC_DISPATCH_WORKDIR
  DZIPC_DISPATCH_RTPRIO
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --user-mode)
            MODE="user"
            shift
            ;;
        --service)
            MODE="service"
            shift
            ;;
        --exec)
            EXEC_PATH="${2:-}"
            MODE="service"
            shift 2
            ;;
        --user)
            RUN_USER="${2:-}"
            shift 2
            ;;
        --workdir)
            WORK_DIR="${2:-}"
            shift 2
            ;;
        --rtprio)
            RTPRIO_LIMIT="${2:-}"
            shift 2
            ;;
        --no-enable)
            ENABLE_SERVICE=0
            shift
            ;;
        --start)
            START_SERVICE=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ "${MODE}" != "user" && "${MODE}" != "service" ]]; then
    echo "Invalid mode: ${MODE}. Expected 'user' or 'service'." >&2
    exit 1
fi

if ! [[ "${RTPRIO_LIMIT}" =~ ^[0-9]+$ ]]; then
    echo "--rtprio must be a non-negative integer." >&2
    exit 1
fi

if [[ -z "${RUN_USER}" ]]; then
    echo "Cannot determine target user. Use --user USER or DZIPC_DISPATCH_USER=USER." >&2
    exit 1
fi

if ! id "${RUN_USER}" >/dev/null 2>&1; then
    echo "Target user does not exist: ${RUN_USER}" >&2
    exit 1
fi

SUDO=()
if [[ "${EUID}" -ne 0 ]]; then
    SUDO=(sudo)
fi

if [[ "${MODE}" == "user" ]]; then
    if [[ -e "${LIMITS_FILE}" ]] && id -nG "${RUN_USER}" | tr ' ' '\n' | grep -qx "${GROUP_NAME}"; then
        echo "Realtime permission profile already exists for ${RUN_USER}, skip installation."
        echo "Limits file: ${LIMITS_FILE}"
        exit 0
    fi

    "${SUDO[@]}" groupadd -f "${GROUP_NAME}"
    "${SUDO[@]}" usermod -aG "${GROUP_NAME}" "${RUN_USER}"

    tmp_limits="$(mktemp)"
    trap 'rm -f "${tmp_limits}"' EXIT
    cat > "${tmp_limits}" <<EOF
@${GROUP_NAME} - rtprio ${RTPRIO_LIMIT}
@${GROUP_NAME} - memlock unlimited
EOF
    "${SUDO[@]}" install -m 0644 -o root -g root "${tmp_limits}" "${LIMITS_FILE}"

    echo "Installed realtime permission profile for all dzipc programs run by ${RUN_USER}."
    echo "Group: ${GROUP_NAME}"
    echo "Limits file: ${LIMITS_FILE}"
    echo "rtprio limit: ${RTPRIO_LIMIT}"
    echo "Log out and log in again, then check with: ulimit -r"
    exit 0
fi

if ! command -v systemctl >/dev/null 2>&1; then
    echo "systemctl is required to install ${UNIT_NAME}." >&2
    exit 1
fi

if systemctl cat "${UNIT_NAME}" >/dev/null 2>&1 || [[ -e "${UNIT_FILE}" ]]; then
    echo "${UNIT_NAME} already exists, skip installation."
    exit 0
fi

if [[ -z "${EXEC_PATH}" ]]; then
    if command -v "${SERVICE_NAME}" >/dev/null 2>&1; then
        EXEC_PATH="$(command -v "${SERVICE_NAME}")"
    elif [[ -x "${PROJECT_DIR}/build/bin/${SERVICE_NAME}" ]]; then
        EXEC_PATH="${PROJECT_DIR}/build/bin/${SERVICE_NAME}"
    elif [[ -x "/usr/local/bin/${SERVICE_NAME}" ]]; then
        EXEC_PATH="/usr/local/bin/${SERVICE_NAME}"
    else
        echo "Cannot find ${SERVICE_NAME}. Use --exec PATH or DZIPC_DISPATCH_BIN=PATH." >&2
        exit 1
    fi
fi

if [[ ! -x "${EXEC_PATH}" ]]; then
    echo "Executable does not exist or is not executable: ${EXEC_PATH}" >&2
    exit 1
fi

if [[ ! -d "${WORK_DIR}" ]]; then
    echo "Working directory does not exist: ${WORK_DIR}" >&2
    exit 1
fi

tmp_unit="$(mktemp)"
trap 'rm -f "${tmp_unit}"' EXIT

cat > "${tmp_unit}" <<EOF
[Unit]
Description=dzIPC dispatch service
After=network.target

[Service]
Type=simple
User=${RUN_USER}
WorkingDirectory=${WORK_DIR}
ExecStart=${EXEC_PATH}
Restart=on-failure
RestartSec=2
AmbientCapabilities=CAP_SYS_NICE
CapabilityBoundingSet=CAP_SYS_NICE
LimitRTPRIO=${RTPRIO_LIMIT}
LimitMEMLOCK=infinity

[Install]
WantedBy=multi-user.target
EOF

"${SUDO[@]}" install -m 0644 -o root -g root "${tmp_unit}" "${UNIT_FILE}"
"${SUDO[@]}" systemctl daemon-reload

if [[ "${ENABLE_SERVICE}" -eq 1 ]]; then
    "${SUDO[@]}" systemctl enable "${UNIT_NAME}"
fi

if [[ "${START_SERVICE}" -eq 1 ]]; then
    "${SUDO[@]}" systemctl start "${UNIT_NAME}"
fi

echo "Installed ${UNIT_NAME}."
echo "Unit file: ${UNIT_FILE}"
echo "ExecStart: ${EXEC_PATH}"
echo "User: ${RUN_USER}"
