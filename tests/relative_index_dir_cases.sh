#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
FIXTURE="${ROOT_DIR}/tests/fixtures/index.txt"

TMP_ROOT="$(mktemp -d)"

cleanup() {
  rm -rf "${TMP_ROOT}"
}
trap cleanup EXIT

echo "[CASE] makeindex: relative index dir should work safely" >&2

mkdir -p "${TMP_ROOT}/pos"
(cd "${TMP_ROOT}" && "${BUILD_DIR}/yappo_makeindex" -f "${FIXTURE}" -d . >/dev/null)
"${BUILD_DIR}/search" -l "${TMP_ROOT}" "OpenAI2025" | grep -q "http://example.com/doc1"

exit 0
