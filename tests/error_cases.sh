#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
FIXTURE="${ROOT_DIR}/tests/fixtures/index.txt"

TMP_ROOT="$(mktemp -d)"
INDEX_DIR_OK="${TMP_ROOT}/ok"
INDEX_DIR_OK2="${TMP_ROOT}/ok2"
INDEX_DIR_OK3="${TMP_ROOT}/ok3"
INDEX_DIR_OK4="${TMP_ROOT}/ok4"
INDEX_DIR_OK5="${TMP_ROOT}/ok5"
INDEX_DIR_OK6="${TMP_ROOT}/ok6"
INDEX_DIR_OK7="${TMP_ROOT}/ok7"
INDEX_DIR_OK8="${TMP_ROOT}/ok8"
INDEX_DIR_OK9="${TMP_ROOT}/ok9"
INDEX_DIR_OK10="${TMP_ROOT}/ok10"
INDEX_DIR_BAD="${TMP_ROOT}/no_pos"
DAEMON_RUN_DIR="${TMP_ROOT}/daemon"
CORE_PID=""
FRONT_PID=""

cleanup() {
  stop_daemons
  rm -rf "${TMP_ROOT}"
}
trap cleanup EXIT

# Case 1: pos/ がないときは失敗すること
mkdir -p "${INDEX_DIR_BAD}"
if "${BUILD_DIR}/yappo_makeindex" -f "${FIXTURE}" -d "${INDEX_DIR_BAD}" >/dev/null 2>&1; then
  echo "Expected yappo_makeindex to fail when pos/ is missing." >&2
  exit 1
fi

assert_not_segv() {
  local index_dir="$1"
  local query="$2"

  set +e
  "${BUILD_DIR}/search" -l "${index_dir}" "${query}" >/dev/null 2>&1
  rc=$?
  set -e

  if [ "${rc}" -eq 139 ]; then
    echo "search crashed with SIGSEGV: index=${index_dir} query=${query}" >&2
    exit 1
  fi
}

make_index() {
  local index_dir="$1"
  mkdir -p "${index_dir}/pos"
  "${BUILD_DIR}/yappo_makeindex" -f "${FIXTURE}" -d "${index_dir}" >/dev/null
}

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

send_http_raw() {
  local payload="$1"
  python3 - "$payload" <<'PY'
import socket, sys
payload = sys.argv[1].encode("utf-8")
s = socket.create_connection(("127.0.0.1", 10080), timeout=1.0)
s.sendall(payload)
try:
    s.recv(4096)
except OSError:
    pass
s.close()
PY
}

send_http_raw_hex() {
  local hex_payload="$1"
  python3 - "$hex_payload" <<'PY'
import socket, sys
payload = bytes.fromhex(sys.argv[1])
s = socket.create_connection(("127.0.0.1", 10080), timeout=1.0)
s.sendall(payload)
try:
    s.recv(4096)
except OSError:
    pass
s.close()
PY
}

send_http_long_query() {
  local length="$1"
  python3 - "$length" <<'PY'
import socket, sys
length = int(sys.argv[1])
query = "A" * length
payload = f"GET / d/100/OR/0-10?{query} HTTP/1.0\r\nHost: localhost\r\n\r\n".encode("utf-8")
s = socket.create_connection(("127.0.0.1", 10080), timeout=1.0)
s.sendall(payload)
try:
    s.recv(4096)
except OSError:
    pass
s.close()
PY
}

assert_daemons_alive() {
  local context="$1"
  if ! kill -0 "${CORE_PID}" 2>/dev/null || ! kill -0 "${FRONT_PID}" 2>/dev/null; then
    echo "front/core daemon crashed: ${context}" >&2
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
  local index_dir="$1"
  mkdir -p "${DAEMON_RUN_DIR}"
  rm -f "${DAEMON_RUN_DIR}/core.pid" "${DAEMON_RUN_DIR}/front.pid"
  (cd "${DAEMON_RUN_DIR}" && "${BUILD_DIR}/yappod_core" -l "${index_dir}" >/dev/null 2>&1)
  (cd "${DAEMON_RUN_DIR}" && "${BUILD_DIR}/yappod_front" -l "${index_dir}" -s 127.0.0.1 >/dev/null 2>&1)

  if [ ! -f "${DAEMON_RUN_DIR}/core.pid" ] || [ ! -f "${DAEMON_RUN_DIR}/front.pid" ]; then
    echo "Failed to start daemon stack (pid file missing)." >&2
    exit 1
  fi
  CORE_PID="$(cat "${DAEMON_RUN_DIR}/core.pid")"
  FRONT_PID="$(cat "${DAEMON_RUN_DIR}/front.pid")"

  if ! wait_for_port 10086 || ! wait_for_port 10080; then
    echo "Failed to open core/front ports." >&2
    exit 1
  fi
}

# Case 2: メタデータ破損時に search が SIGSEGV しないこと
make_index "${INDEX_DIR_OK}"

# Case 2-1: keyword_docsnum を破損（短い読み込み）
: > "${INDEX_DIR_OK}/keyword_docsnum"
assert_not_segv "${INDEX_DIR_OK}" "テスト"

# Case 2-2: keyword_totalnum を破損（短い読み込み）
make_index "${INDEX_DIR_OK2}"
: > "${INDEX_DIR_OK2}/keyword_totalnum"
assert_not_segv "${INDEX_DIR_OK2}" "テスト"

# Case 2-3: pos ヘッダを破損（先頭posファイルをtruncate）
make_index "${INDEX_DIR_OK3}"
POS_FILE="$(find "${INDEX_DIR_OK3}/pos" -maxdepth 1 -type f | grep -E '/[0-9]+$' | head -n 1 || true)"
if [ -z "${POS_FILE}" ]; then
  echo "Could not find postings file under ${INDEX_DIR_OK3}/pos" >&2
  exit 1
fi
truncate -s 4 "${POS_FILE}"
assert_not_segv "${INDEX_DIR_OK3}" "テスト"

# Case 2-4: size を破損（短い読み込み）
make_index "${INDEX_DIR_OK4}"
truncate -s 1 "${INDEX_DIR_OK4}/size"
assert_not_segv "${INDEX_DIR_OK4}" "テスト"

# Case 2-5: domainid を破損（短い読み込み）
make_index "${INDEX_DIR_OK5}"
truncate -s 1 "${INDEX_DIR_OK5}/domainid"
assert_not_segv "${INDEX_DIR_OK5}" "テスト"

# Case 2-6: score を破損（短い読み込み）
make_index "${INDEX_DIR_OK6}"
truncate -s 1 "${INDEX_DIR_OK6}/score"
assert_not_segv "${INDEX_DIR_OK6}" "テスト"

# Case 2-7: 不正な%エスケープを含むクエリで search が SIGSEGV しないこと
make_index "${INDEX_DIR_OK7}"
assert_not_segv "${INDEX_DIR_OK7}" "%"
assert_not_segv "${INDEX_DIR_OK7}" "%A"
assert_not_segv "${INDEX_DIR_OK7}" "%ZZ"
assert_not_segv "${INDEX_DIR_OK7}" "A%2"

# Case 2-8: 破損インデックスでも front/core 経由でプロセスが落ちないこと
make_index "${INDEX_DIR_OK8}"
truncate -s 1 "${INDEX_DIR_OK8}/score"
start_daemons "${INDEX_DIR_OK8}"
send_http_raw $'GET / bad\r\n\r\n'
send_http_raw $'GET / d/100/OR/0-10?OpenAI2025 HTTP/1.0\r\nHost: localhost\r\n\r\n'
assert_daemons_alive "malformed or broken-index request"
stop_daemons

# Case 2-9: core 停止後のリクエストでも front が落ちないこと
make_index "${INDEX_DIR_OK9}"
start_daemons "${INDEX_DIR_OK9}"
kill "${CORE_PID}" 2>/dev/null || true
sleep 0.2
send_http_raw $'GET / d/100/OR/0-10?OpenAI2025 HTTP/1.0\r\nHost: localhost\r\n\r\n'
if ! kill -0 "${FRONT_PID}" 2>/dev/null; then
  echo "front daemon crashed after core disconnect." >&2
  exit 1
fi
stop_daemons

# Case 2-10: 巨大クエリ行でも front/core が落ちないこと
make_index "${INDEX_DIR_OK10}"
start_daemons "${INDEX_DIR_OK10}"
send_http_long_query 200000
assert_daemons_alive "oversized request query"
stop_daemons

# Case 2-11: 不正UTF-8バイト列を含むHTTPクエリでも front/core が落ちないこと
make_index "${INDEX_DIR_OK10}"
start_daemons "${INDEX_DIR_OK10}"
send_http_raw_hex "474554202f20642f3130302f4f522f302d31303fffFE20485454502f312e300d0a486f73743a206c6f63616c686f73740d0a0d0a"
assert_daemons_alive "invalid utf-8 query bytes"
stop_daemons

# Case 2-12: 途中で切れたUTF-8バイト列を含むHTTPクエリでも front/core が落ちないこと
make_index "${INDEX_DIR_OK10}"
start_daemons "${INDEX_DIR_OK10}"
send_http_raw_hex "474554202f20642f3130302f4f522f302d31303fe38120485454502f312e300d0a486f73743a206c6f63616c686f73740d0a0d0a"
assert_daemons_alive "truncated utf-8 query bytes"
stop_daemons

exit 0
