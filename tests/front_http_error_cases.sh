#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
FIXTURE="${ROOT_DIR}/tests/fixtures/index.txt"

# shellcheck source=tests/test_helpers.sh
source "${ROOT_DIR}/tests/test_helpers.sh"

TMP_ROOT="$(mktemp -d)"
INDEX_DIR="${TMP_ROOT}/index"
DAEMON_RUN_DIR="${TMP_ROOT}/daemon"
CORE_PID=""
FRONT_PID=""
CURRENT_CASE="setup"

cleanup() {
  stop_daemons
  rm -rf "${TMP_ROOT}"
}
trap cleanup EXIT

case_begin() {
  CURRENT_CASE="$1"
  echo "[CASE] ${CURRENT_CASE}" >&2
}

dump_daemon_logs() {
  local f

  if [ ! -d "${DAEMON_RUN_DIR}" ]; then
    return
  fi

  echo "[DEBUG] daemon run dir: ${DAEMON_RUN_DIR}" >&2
  for f in core.pid front.pid core.log core.error front.log front.error; do
    if [ -f "${DAEMON_RUN_DIR}/${f}" ]; then
      echo "[DEBUG] ----- ${f} -----" >&2
      cat "${DAEMON_RUN_DIR}/${f}" >&2 || true
      echo >&2
      echo "[DEBUG] --------------------" >&2
    fi
  done
}

on_error() {
  local rc=$?
  echo "[ERROR] case='${CURRENT_CASE}' rc=${rc}" >&2
  dump_daemon_logs
  dump_sanitizer_logs
}
trap on_error ERR

wait_for_port() {
  local port="$1"
  local retries=50
  local i
  for ((i=0; i<retries; i++)); do
    if python3 - "$port" <<'PY'
import socket, sys
port = int(sys.argv[1])
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(0.1)
try:
    s.connect(("127.0.0.1", port))
except OSError:
    sys.exit(1)
else:
    s.close()
    sys.exit(0)
PY
    then
      return 0
    fi
    sleep 0.1
  done
  return 1
}

send_http_capture() {
  local payload="$1"
  python3 - "$payload" <<'PY'
import socket, sys
payload = sys.argv[1].encode("utf-8")
s = socket.create_connection(("127.0.0.1", 10080), timeout=1.0)
s.sendall(payload)
s.shutdown(socket.SHUT_WR)
chunks = []
while True:
    try:
        chunk = s.recv(4096)
    except OSError:
        break
    if not chunk:
        break
    chunks.append(chunk)
s.close()
sys.stdout.buffer.write(b"".join(chunks))
PY
}

send_http_long_header_capture() {
  local length="$1"
  python3 - "$length" <<'PY'
import socket, sys
length = int(sys.argv[1])
header = "A" * length
payload = (
    "GET /yappo/100000/AND/0-10?OpenAI2025 HTTP/1.1\r\n"
    "Host: localhost\r\n"
    f"X-Long: {header}\r\n"
    "\r\n"
).encode("utf-8")
s = socket.create_connection(("127.0.0.1", 10080), timeout=1.0)
s.sendall(payload)
s.shutdown(socket.SHUT_WR)
chunks = []
while True:
    try:
        chunk = s.recv(4096)
    except OSError:
        break
    if not chunk:
        break
    chunks.append(chunk)
s.close()
sys.stdout.buffer.write(b"".join(chunks))
PY
}

assert_daemons_alive() {
  local context="$1"
  if ! kill -0 "${CORE_PID}" 2>/dev/null || ! kill -0 "${FRONT_PID}" 2>/dev/null; then
    echo "front/core daemon crashed: ${context}" >&2
    dump_daemon_logs
    dump_sanitizer_logs
    exit 1
  fi
}

expect_bad_request() {
  local response="$1"
  local context="$2"
  if ! echo "${response}" | grep -q "400 Bad Request"; then
    echo "Expected 400 Bad Request for case: ${context}" >&2
    echo "${response}" >&2
    exit 1
  fi
}

stop_daemons() {
  local pid
  for pid in "${FRONT_PID}" "${CORE_PID}"; do
    if [ -n "${pid}" ] && kill -0 "${pid}" 2>/dev/null; then
      kill "${pid}" 2>/dev/null || true
      sleep 0.1
      if kill -0 "${pid}" 2>/dev/null; then
        kill -9 "${pid}" 2>/dev/null || true
      fi
    fi
  done
  FRONT_PID=""
  CORE_PID=""
}

start_daemons() {
  mkdir -p "${DAEMON_RUN_DIR}"
  rm -f "${DAEMON_RUN_DIR}/core.pid" "${DAEMON_RUN_DIR}/front.pid"
  (cd "${DAEMON_RUN_DIR}" && "${BUILD_DIR}/yappod_core" -l "${INDEX_DIR}" >/dev/null 2>&1)
  (cd "${DAEMON_RUN_DIR}" && "${BUILD_DIR}/yappod_front" -l "${INDEX_DIR}" -s 127.0.0.1 >/dev/null 2>&1)

  if [ ! -f "${DAEMON_RUN_DIR}/core.pid" ] || [ ! -f "${DAEMON_RUN_DIR}/front.pid" ]; then
    echo "Failed to start daemon stack (pid file missing)." >&2
    dump_daemon_logs
    dump_sanitizer_logs
    exit 1
  fi
  CORE_PID="$(cat "${DAEMON_RUN_DIR}/core.pid")"
  FRONT_PID="$(cat "${DAEMON_RUN_DIR}/front.pid")"
  if ! kill -0 "${CORE_PID}" 2>/dev/null || ! kill -0 "${FRONT_PID}" 2>/dev/null; then
    echo "Failed to start daemon stack (process exited)." >&2
    dump_daemon_logs
    dump_sanitizer_logs
    exit 1
  fi

  if ! wait_for_port 10086 || ! wait_for_port 10080; then
    echo "Failed to open core/front ports." >&2
    dump_daemon_logs
    dump_sanitizer_logs
    exit 1
  fi
}

case_begin "front HTTP malformed request handling"

mkdir -p "${INDEX_DIR}/pos"
"${BUILD_DIR}/yappo_makeindex" -f "${FIXTURE}" -d "${INDEX_DIR}" >/dev/null
start_daemons

RESP="$(send_http_capture $'POST /yappo/100000/AND/0-10?OpenAI2025 HTTP/1.1\r\nHost: localhost\r\n\r\n')"
expect_bad_request "${RESP}" "invalid method"
assert_daemons_alive "invalid method"

RESP="$(send_http_capture $'GET /yappo/100000/AND/0-10?OpenAI2025\r\nHost: localhost\r\n\r\n')"
expect_bad_request "${RESP}" "missing HTTP version"
assert_daemons_alive "missing version"

RESP="$(send_http_capture $'GET /yappo/100000/AND/0-10 HTTP/1.1\r\nHost: localhost\r\n\r\n')"
expect_bad_request "${RESP}" "missing query"
assert_daemons_alive "missing query"

RESP="$(send_http_capture $'GET /yappo/100000/AND/notrange?OpenAI2025 HTTP/1.1\r\nHost: localhost\r\n\r\n')"
expect_bad_request "${RESP}" "invalid range"
assert_daemons_alive "invalid range"

RESP="$(send_http_long_header_capture 20000)"
expect_bad_request "${RESP}" "oversized header"
assert_daemons_alive "oversized header"

stop_daemons
exit 0
