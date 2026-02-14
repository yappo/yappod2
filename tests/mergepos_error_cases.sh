#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
FIXTURE="${ROOT_DIR}/tests/fixtures/index.txt"
TMP_ROOT="$(mktemp -d)"
INDEX_DIR="${TMP_ROOT}/index"
CURRENT_CASE="setup"

# shellcheck source=tests/test_helpers.sh
source "${ROOT_DIR}/tests/test_helpers.sh"

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

mkdir -p "${INDEX_DIR}/pos"
"${BUILD_DIR}/yappo_makeindex" -f "${FIXTURE}" -d "${INDEX_DIR}" >/dev/null

run_expect_fail() {
  local case_name="$1"
  local expect="$2"
  shift 2
  local out
  local rc

  CURRENT_CASE="${case_name}"
  echo "[CASE] ${case_name}" >&2
  set +e
  out="$("$@" 2>&1)"
  rc=$?
  set -e

  if [ "${rc}" -eq 0 ]; then
    echo "Expected non-zero exit: ${case_name}" >&2
    exit 1
  fi
  if ! echo "${out}" | grep -Fq "${expect}"; then
    echo "Expected message not found: ${case_name}" >&2
    echo "expected: ${expect}" >&2
    echo "actual:" >&2
    echo "${out}" >&2
    exit 1
  fi
}

run_expect_fail \
  "mergepos: missing required -d option should fail" \
  "Missing required option: -d output_file" \
  "${BUILD_DIR}/yappo_mergepos" -l "${INDEX_DIR}" -s 0 -e 0

run_expect_fail \
  "mergepos: missing required -l option should fail" \
  "Missing required option: -l input_index" \
  "${BUILD_DIR}/yappo_mergepos" -d "${TMP_ROOT}/outpos" -s 0 -e 0

run_expect_fail \
  "mergepos: invalid -s should fail" \
  "Invalid start value: -1" \
  "${BUILD_DIR}/yappo_mergepos" -l "${INDEX_DIR}" -d "${TMP_ROOT}/outpos" -s -1 -e 0

run_expect_fail \
  "mergepos: invalid -e should fail" \
  "Invalid end value: abc" \
  "${BUILD_DIR}/yappo_mergepos" -l "${INDEX_DIR}" -d "${TMP_ROOT}/outpos" -s 0 -e abc

run_expect_fail \
  "mergepos: start/end range inversion should fail" \
  "pos num error: 0 - 1 = 0" \
  "${BUILD_DIR}/yappo_mergepos" -l "${INDEX_DIR}" -d "${TMP_ROOT}/outpos" -s 1 -e 0

run_expect_fail \
  "mergepos: missing pos shard range should fail" \
  "Missing required pos shard file: ${INDEX_DIR}/pos/1" \
  "${BUILD_DIR}/yappo_mergepos" -l "${INDEX_DIR}" -d "${TMP_ROOT}/outpos" -s 0 -e 1

run_expect_fail \
  "mergepos: non-existing input dir should fail" \
  "Please specify an existing index directory." \
  "${BUILD_DIR}/yappo_mergepos" -l "${TMP_ROOT}/no_such_dir" -d "${TMP_ROOT}/outpos" -s 0 -e 0

INDEX_NO_DELETE="${TMP_ROOT}/index_no_delete"
cp -R "${INDEX_DIR}" "${INDEX_NO_DELETE}"
rm -f "${INDEX_NO_DELETE}/deletefile"
run_expect_fail \
  "mergepos: missing deletefile should fail" \
  "fopen error: ${INDEX_NO_DELETE}/deletefile" \
  "${BUILD_DIR}/yappo_mergepos" -l "${INDEX_NO_DELETE}" -d "${TMP_ROOT}/outpos_no_delete" -s 0 -e 0

INDEX_NO_KEYNUM="${TMP_ROOT}/index_no_keynum"
cp -R "${INDEX_DIR}" "${INDEX_NO_KEYNUM}"
rm -f "${INDEX_NO_KEYNUM}/keywordnum"
run_expect_fail \
  "mergepos: missing keywordnum should fail" \
  "fopen error: ${INDEX_NO_KEYNUM}/keywordnum" \
  "${BUILD_DIR}/yappo_mergepos" -l "${INDEX_NO_KEYNUM}" -d "${TMP_ROOT}/outpos_no_keynum" -s 0 -e 0

INDEX_MALFORMED="${TMP_ROOT}/index_malformed_postings"
cp -R "${INDEX_DIR}" "${INDEX_MALFORMED}"
python3 - "${INDEX_MALFORMED}" <<'PY'
import os
import struct
import sys

base = sys.argv[1]
keywordnum_path = os.path.join(base, "keywordnum")
index_path = os.path.join(base, "pos", "0_index")
size_path = os.path.join(base, "pos", "0_size")
data_path = os.path.join(base, "pos", "0")

with open(keywordnum_path, "rb") as f:
    data = f.read(4)
if len(data) != 4:
    sys.exit(0)
keyword_num = struct.unpack("<i", data)[0]
if keyword_num <= 0:
    sys.exit(0)

vals = [1, 1000000]
enc = bytearray()
for v in vals:
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            enc.append(b | 0x80)
        else:
            enc.append(b)
            break

with open(index_path, "r+b") as fidx, open(size_path, "r+b") as fsize, open(data_path, "r+b") as fdata:
    for kid in range(1, keyword_num + 1):
        fidx.seek(4 * kid)
        raw = fidx.read(4)
        if len(raw) != 4:
            break
        idx = struct.unpack("<i", raw)[0]
        if idx < 0:
            continue
        fdata.seek(idx)
        fdata.write(enc)
        fsize.seek(4 * kid)
        fsize.write(struct.pack("<i", len(enc)))
PY
run_expect_fail \
  "mergepos: malformed postings payload should fail safely" \
  "malformed postings payload:" \
  "${BUILD_DIR}/yappo_mergepos" -l "${INDEX_MALFORMED}" -d "${TMP_ROOT}/outpos_malformed" -s 0 -e 0

exit 0
