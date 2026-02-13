#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

TMP_ROOT="$(mktemp -d)"
INPUT_DIR="${TMP_ROOT}/input"
INDEX_DIR="${TMP_ROOT}/index"

cleanup() {
  if [ -d "${INPUT_DIR}" ]; then
    chmod 700 "${INPUT_DIR}" 2>/dev/null || true
  fi
  rm -rf "${TMP_ROOT}"
}
trap cleanup EXIT

echo "[CASE] makeindex: opendir failure should return error" >&2

mkdir -p "${INPUT_DIR}"
mkdir -p "${INDEX_DIR}/pos"
chmod 000 "${INPUT_DIR}"

set +e
OUT="$("${BUILD_DIR}/yappo_makeindex" -l "${INPUT_DIR}" -d "${INDEX_DIR}" 2>&1)"
RC=$?
set -e

if [ "${RC}" -eq 0 ]; then
  echo "Expected non-zero exit when opendir fails." >&2
  exit 1
fi

if ! echo "${OUT}" | grep -Fq "ERROR: opendir failed"; then
  echo "Expected opendir failure message, got:" >&2
  echo "${OUT}" >&2
  exit 1
fi

exit 0
