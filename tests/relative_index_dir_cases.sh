#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
FIXTURE="${ROOT_DIR}/tests/fixtures/index.txt"
CURRENT_CASE="setup"

# shellcheck source=tests/test_helpers.sh
source "${ROOT_DIR}/tests/test_helpers.sh"

TMP_ROOT="$(mktemp -d)"

cleanup() {
  rm -rf "${TMP_ROOT}"
}
trap cleanup EXIT

on_error() {
  local rc=$?
  echo "[ERROR] case='${CURRENT_CASE}' rc=${rc} cmd='${BASH_COMMAND}'" >&2
  dump_sanitizer_logs
}
trap on_error ERR

CURRENT_CASE="makeindex: relative index dir should work safely"
echo "[CASE] ${CURRENT_CASE}" >&2

mkdir -p "${TMP_ROOT}/pos"
(cd "${TMP_ROOT}" && "${BUILD_DIR}/yappo_makeindex" -f "${FIXTURE}" -d . >/dev/null)
"${BUILD_DIR}/search" -l "${TMP_ROOT}" "OpenAI2025" | grep -q "http://example.com/doc1"

exit 0
