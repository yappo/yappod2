#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

echo "[CASE] mergepos: missing required -d option should fail" >&2

set +e
OUT="$("${BUILD_DIR}/yappo_mergepos" -l /tmp -s 0 -e 0 2>&1)"
RC=$?
set -e

if [ "${RC}" -eq 0 ]; then
  echo "Expected non-zero exit when -d is missing." >&2
  exit 1
fi

if ! echo "${OUT}" | grep -Fq "Missing required option: -d output_file"; then
  echo "Expected error message for missing -d, got:" >&2
  echo "${OUT}" >&2
  exit 1
fi
