#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
FIXTURE="${ROOT_DIR}/tests/fixtures/index.txt"

TMP_ROOT="$(mktemp -d)"
INDEX_DIR_OK="${TMP_ROOT}/ok"
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

# Case 2: メタデータ破損時に search が SIGSEGV しないこと
mkdir -p "${INDEX_DIR_OK}/pos"
"${BUILD_DIR}/yappo_makeindex" -f "${FIXTURE}" -d "${INDEX_DIR_OK}" >/dev/null

# keyword_docsnum を破損させる（短い読み込みを誘発）
: > "${INDEX_DIR_OK}/keyword_docsnum"

set +e
"${BUILD_DIR}/search" -l "${INDEX_DIR_OK}" "テスト" >/dev/null 2>&1
rc=$?
set -e

if [ "${rc}" -eq 139 ]; then
  echo "search crashed with SIGSEGV on corrupted metadata." >&2
  exit 1
fi

exit 0
