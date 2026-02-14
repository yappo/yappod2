#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
FIXTURE="${ROOT_DIR}/tests/fixtures/index.txt"
FIXTURE_MALFORMED="${ROOT_DIR}/tests/fixtures/index_malformed.txt"

# shellcheck source=tests/test_helpers.sh
source "${ROOT_DIR}/tests/test_helpers.sh"

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
INDEX_DIR_OK11="${TMP_ROOT}/ok11"
INDEX_DIR_OK12="${TMP_ROOT}/ok12"
INDEX_DIR_OK13="${TMP_ROOT}/ok13"
INDEX_DIR_OK14="${TMP_ROOT}/ok14"
INDEX_DIR_OK15="${TMP_ROOT}/ok15"
INDEX_DIR_OK16="${TMP_ROOT}/ok16"
INDEX_DIR_BAD="${TMP_ROOT}/no_pos"
DAEMON_RUN_DIR="${TMP_ROOT}/daemon"
CORE_PID=""
FRONT_PID=""

cleanup() {
  stop_daemons
  rm -rf "${TMP_ROOT}"
}
trap cleanup EXIT

CURRENT_CASE="setup"

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

# Case 1: pos/ がないときは失敗すること
case_begin "Case 1: missing pos directory should fail"
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

send_http_long_query_capture() {
  local length="$1"
  python3 - "$length" <<'PY'
import socket, sys
length = int(sys.argv[1])
query = "A" * length
payload = f"GET /d/100/OR/0-10?{query} HTTP/1.0\r\nHost: localhost\r\n\r\n".encode("utf-8")
s = socket.create_connection(("127.0.0.1", 10080), timeout=1.0)
s.sendall(payload)
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

# Case 2: メタデータ破損時に search が SIGSEGV しないこと
case_begin "Case 2-1: broken keyword_docsnum"
make_index "${INDEX_DIR_OK}"

# Case 2-1: keyword_docsnum を破損（短い読み込み）
: > "${INDEX_DIR_OK}/keyword_docsnum"
assert_not_segv "${INDEX_DIR_OK}" "テスト"

# Case 2-2: keyword_totalnum を破損（短い読み込み）
case_begin "Case 2-2: broken keyword_totalnum"
make_index "${INDEX_DIR_OK2}"
: > "${INDEX_DIR_OK2}/keyword_totalnum"
assert_not_segv "${INDEX_DIR_OK2}" "テスト"

# Case 2-3: pos ヘッダを破損（先頭posファイルをtruncate）
case_begin "Case 2-3: truncated postings header"
make_index "${INDEX_DIR_OK3}"
POS_FILE="$(find "${INDEX_DIR_OK3}/pos" -maxdepth 1 -type f | grep -E '/[0-9]+$' | head -n 1 || true)"
if [ -z "${POS_FILE}" ]; then
  echo "Could not find postings file under ${INDEX_DIR_OK3}/pos" >&2
  exit 1
fi
truncate -s 4 "${POS_FILE}"
assert_not_segv "${INDEX_DIR_OK3}" "テスト"

# Case 2-4: size を破損（短い読み込み）
case_begin "Case 2-4: truncated size file"
make_index "${INDEX_DIR_OK4}"
truncate -s 1 "${INDEX_DIR_OK4}/size"
assert_not_segv "${INDEX_DIR_OK4}" "テスト"

# Case 2-5: domainid を破損（短い読み込み）
case_begin "Case 2-5: truncated domainid file"
make_index "${INDEX_DIR_OK5}"
truncate -s 1 "${INDEX_DIR_OK5}/domainid"
assert_not_segv "${INDEX_DIR_OK5}" "テスト"

# Case 2-6: score を破損（短い読み込み）
case_begin "Case 2-6: truncated score file"
make_index "${INDEX_DIR_OK6}"
truncate -s 1 "${INDEX_DIR_OK6}/score"
assert_not_segv "${INDEX_DIR_OK6}" "テスト"

# Case 2-7: 不正な%エスケープを含むクエリで search が SIGSEGV しないこと
case_begin "Case 2-7: invalid percent escapes"
make_index "${INDEX_DIR_OK7}"
assert_not_segv "${INDEX_DIR_OK7}" "%"
assert_not_segv "${INDEX_DIR_OK7}" "%A"
assert_not_segv "${INDEX_DIR_OK7}" "%ZZ"
assert_not_segv "${INDEX_DIR_OK7}" "A%2"

# Case 2-8: 破損インデックスでも front/core 経由でプロセスが落ちないこと
case_begin "Case 2-8: broken index over daemon path"
make_index "${INDEX_DIR_OK8}"
truncate -s 1 "${INDEX_DIR_OK8}/score"
start_daemons "${INDEX_DIR_OK8}"
send_http_raw $'GET / bad\r\n\r\n'
send_http_raw $'GET /d/100/OR/0-10?OpenAI2025 HTTP/1.0\r\nHost: localhost\r\n\r\n'
assert_daemons_alive "malformed or broken-index request"
stop_daemons

# Case 2-9: core 停止後のリクエストでも front が落ちないこと
case_begin "Case 2-9: front survives core disconnect"
make_index "${INDEX_DIR_OK9}"
start_daemons "${INDEX_DIR_OK9}"
kill "${CORE_PID}" 2>/dev/null || true
sleep 0.2
send_http_raw $'GET /d/100/OR/0-10?OpenAI2025 HTTP/1.0\r\nHost: localhost\r\n\r\n'
if ! kill -0 "${FRONT_PID}" 2>/dev/null; then
  echo "front daemon crashed after core disconnect." >&2
  exit 1
fi
stop_daemons

# Case 2-10: 巨大クエリ行でも front/core が落ちないこと
case_begin "Case 2-10: oversized query"
make_index "${INDEX_DIR_OK10}"
start_daemons "${INDEX_DIR_OK10}"
RESP="$(send_http_long_query_capture 200000)"
echo "${RESP}" | grep -q "400 Bad Request"
assert_daemons_alive "oversized request query"
stop_daemons

# Case 2-11: 不正UTF-8バイト列を含むHTTPクエリでも front/core が落ちないこと
case_begin "Case 2-11: invalid utf-8 bytes"
make_index "${INDEX_DIR_OK15}"
start_daemons "${INDEX_DIR_OK15}"
send_http_raw_hex "474554202f642f3130302f4f522f302d31303ffffe20485454502f312e300d0a486f73743a206c6f63616c686f73740d0a0d0a"
assert_daemons_alive "invalid utf-8 query bytes"
stop_daemons

# Case 2-12: 途中で切れたUTF-8バイト列を含むHTTPクエリでも front/core が落ちないこと
case_begin "Case 2-12: truncated utf-8 bytes"
make_index "${INDEX_DIR_OK16}"
start_daemons "${INDEX_DIR_OK16}"
send_http_raw_hex "474554202f642f3130302f4f522f302d31303fe38120485454502f312e300d0a486f73743a206c6f63616c686f73740d0a0d0a"
assert_daemons_alive "truncated utf-8 query bytes"
stop_daemons

# Case 2-13: 標準HTTPパス形式(/dict/...)でも検索できること
case_begin "Case 2-13: standard HTTP path"
make_index "${INDEX_DIR_OK11}"
start_daemons "${INDEX_DIR_OK11}"
RESP="$(send_http_capture $'GET /yappo/100000/AND/0-10?OpenAI2025 HTTP/1.1\r\nHost: localhost\r\n\r\n')"
echo "${RESP}" | grep -q "http://example.com/doc1"
assert_daemons_alive "standard-path request"
stop_daemons

# Case 2-14: 不正行を含む入力でも有効行の索引を継続すること
case_begin "Case 2-14: malformed input rows are skipped"
mkdir -p "${INDEX_DIR_OK12}/pos"
"${BUILD_DIR}/yappo_makeindex" -f "${FIXTURE_MALFORMED}" -d "${INDEX_DIR_OK12}" >/dev/null
"${BUILD_DIR}/search" -l "${INDEX_DIR_OK12}" "OpenAI2025" | grep -q "http://example.com/doc1"
"${BUILD_DIR}/search" -l "${INDEX_DIR_OK12}" "検索用のテスト本文です" | grep -q "http://example.com/doc2"
"${BUILD_DIR}/search" -l "${INDEX_DIR_OK12}" "badcmdtoken999" | grep -q "Hit num: 0\\|not found"

# Case 2-15: 長大行・BODY_SIZE不整合・同一バッチ重複ADDがあっても処理継続すること
case_begin "Case 2-15: oversized and malformed rows do not break indexing"
INPUT_EDGE="${TMP_ROOT}/index_edge_cases.txt"
mkdir -p "${INDEX_DIR_OK13}/pos"
{
  printf "http://example.com/ok1\tADD\tOK1\t24\taaaaaaaaaaaaaaaaaaaaaaaa\n"
  printf "http://example.com/badsize\tADD\tBadSize\t999999999999\tbadsizepayload\n"
  printf "http://example.com/dup\tADD\tDup\t24\tbbbbbbbbbbbbbbbbbbbbbbbb\n"
  printf "http://example.com/dup\tADD\tDup2\t24\tbbbbbbbbbbbbbbbbbbbbbbbb\n"
  printf "http://example.com/ok2\tADD\tOK2\t24\tcccccccccccccccccccccccc\n"
  printf "http://example.com/long\tADD\tLong\t24\t"
  head -c 1100000 /dev/zero | tr '\0' 'L'
  printf "\n"
  printf "http://example.com/ok3\tADD\tOK3\t24\tdddddddddddddddddddddddd\n"
  printf "http://example.com/ok4\tADD\tOK4\t24\teeeeeeeeeeeeeeeeeeeeeeee\n"
} > "${INPUT_EDGE}"
"${BUILD_DIR}/yappo_makeindex" -f "${INPUT_EDGE}" -d "${INDEX_DIR_OK13}" >/dev/null
"${BUILD_DIR}/search" -l "${INDEX_DIR_OK13}" "aaaaaaaaaaaaaaaaaaaaaaaa" | grep -q "http://example.com/ok1"
"${BUILD_DIR}/search" -l "${INDEX_DIR_OK13}" "cccccccccccccccccccccccc" | grep -q "http://example.com/ok2"
"${BUILD_DIR}/search" -l "${INDEX_DIR_OK13}" "dddddddddddddddddddddddd" | grep -q "http://example.com/ok3"
"${BUILD_DIR}/search" -l "${INDEX_DIR_OK13}" "badsizepayload" | grep -q "Hit num: 0\\|not found"
"${BUILD_DIR}/search" -l "${INDEX_DIR_OK13}" "Dup2" | grep -q "Hit num: 0\\|not found"

# Case 2-16: 不正%エスケープを含むHTTPクエリでも front/core が落ちないこと
case_begin "Case 2-16: invalid percent escapes over daemon path"
make_index "${INDEX_DIR_OK14}"
start_daemons "${INDEX_DIR_OK14}"
RESP="$(send_http_capture $'GET /yappo/100000/AND/0-10?% HTTP/1.1\r\nHost: localhost\r\n\r\n')"
echo "${RESP}" | grep -q "HTTP/1.0 200 OK"
RESP="$(send_http_capture $'GET /yappo/100000/AND/0-10?%A HTTP/1.1\r\nHost: localhost\r\n\r\n')"
echo "${RESP}" | grep -q "HTTP/1.0 200 OK"
RESP="$(send_http_capture $'GET /yappo/100000/AND/0-10?%ZZ HTTP/1.1\r\nHost: localhost\r\n\r\n')"
echo "${RESP}" | grep -q "HTTP/1.0 200 OK"
RESP="$(send_http_capture $'GET /yappo/100000/AND/0-10?A%2 HTTP/1.1\r\nHost: localhost\r\n\r\n')"
echo "${RESP}" | grep -q "HTTP/1.0 200 OK"
assert_daemons_alive "invalid-percent request"
stop_daemons

exit 0
