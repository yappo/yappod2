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
INDEX_DIR_BAD="${TMP_ROOT}/no_pos"

cleanup() {
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

exit 0
