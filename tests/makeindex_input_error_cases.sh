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

case_begin() {
  CURRENT_CASE="$1"
  echo "[CASE] ${CURRENT_CASE}" >&2
}

on_error() {
  local rc=$?
  echo "[ERROR] case='${CURRENT_CASE}' rc=${rc} cmd='${BASH_COMMAND}'" >&2
  dump_sanitizer_logs
}
trap on_error ERR

make_index() {
  local input_file="$1"
  local index_dir="$2"
  shift 2
  mkdir -p "${index_dir}/pos"
  "${BUILD_DIR}/yappo_makeindex" -f "${input_file}" -d "${index_dir}" "$@" >/dev/null
}

expect_hit() {
  local index_dir="$1"
  local query="$2"
  local expect="$3"
  if ! "${BUILD_DIR}/search" -l "${index_dir}" "${query}" | grep -q "${expect}"; then
    echo "Expected hit '${expect}' for query '${query}' in ${index_dir}" >&2
    exit 1
  fi
}

expect_no_hit() {
  local index_dir="$1"
  local query="$2"
  if ! "${BUILD_DIR}/search" -l "${index_dir}" "${query}" | grep -q "Hit num: 0\\|not found"; then
    echo "Expected no hit for query '${query}' in ${index_dir}" >&2
    exit 1
  fi
}

case_begin "makeindex: invalid URL should not break indexing"
INPUT1="${TMP_ROOT}/input_invalid_url.txt"
INDEX1="${TMP_ROOT}/index1"
sed -n '1,2p' "${FIXTURE}" > "${INPUT1}"
cat >> "${INPUT1}" <<'EOF'
not_a_url	ADD	BadURL	100	この行はURL形式不正ですが処理継続の確認用です。
EOF
sed -n '3,$p' "${FIXTURE}" >> "${INPUT1}"
make_index "${INPUT1}" "${INDEX1}"
expect_hit "${INDEX1}" "OpenAI2025" "http://example.com/doc1"
expect_hit "${INDEX1}" "abc123" "http://example.com/doc5"

case_begin "makeindex: empty/broken rows are skipped and processing continues"
INPUT2="${TMP_ROOT}/input_broken_rows.txt"
INDEX2="${TMP_ROOT}/index2"
sed -n '1,3p' "${FIXTURE}" > "${INPUT2}"
cat >> "${INPUT2}" <<'EOF'

MALFORMED LINE WITHOUT TABS
http://example.com/missingbody	ADD	OnlyTitle	100
	ADD	NoUrl	100	nourltoken222xxxyyyzzz
http://example.com/badcmd	UPSERT	BadCmd	100	badcmdtoken333mmmnnnooo
EOF
sed -n '4,$p' "${FIXTURE}" >> "${INPUT2}"
make_index "${INPUT2}" "${INDEX2}"
expect_hit "${INDEX2}" "OpenAI2025" "http://example.com/doc1"
expect_hit "${INDEX2}" "abc123" "http://example.com/doc5"
expect_no_hit "${INDEX2}" "badcmdtoken333mmmnnnooo"
expect_no_hit "${INDEX2}" "nourltoken222xxxyyyzzz"

case_begin "makeindex: oversized burst lines are skipped and valid lines survive"
INPUT3="${TMP_ROOT}/input_oversized_burst.txt"
INDEX3="${TMP_ROOT}/index3"
sed -n '1,4p' "${FIXTURE}" > "${INPUT3}"
python3 - "${INPUT3}" <<'PY'
import sys
path = sys.argv[1]
huge = "X" * 1100000
with open(path, "a", encoding="utf-8") as f:
    f.write("http://example.com/huge1\tADD\tHuge1\t100\toversizedtoken666lllmmmnnn" + huge + "\n")
    f.write("http://example.com/huge2\tADD\tHuge2\t100\toversizedtoken777ooopppqqq" + huge + "\n")
PY
sed -n '5,$p' "${FIXTURE}" >> "${INPUT3}"
make_index "${INPUT3}" "${INDEX3}"
expect_hit "${INDEX3}" "OpenAI2025" "http://example.com/doc1"
expect_hit "${INDEX3}" "abc123" "http://example.com/doc5"
expect_no_hit "${INDEX3}" "削除対象"
expect_no_hit "${INDEX3}" "oversizedtoken666lllmmmnnn"
expect_no_hit "${INDEX3}" "oversizedtoken777ooopppqqq"

case_begin "makeindex: invalid body_size fields are skipped"
INPUT4="${TMP_ROOT}/input_invalid_size_fields.txt"
INDEX4="${TMP_ROOT}/index4"
sed -n '1,2p' "${FIXTURE}" > "${INPUT4}"
cat >> "${INPUT4}" <<'EOF'
http://example.com/badsize-neg	ADD	BadSizeNeg	-1	badsizeNEGtoken888aaabbb
http://example.com/badsize-over	ADD	BadSizeOver	2147483648	badsizeOVERtoken999cccddd
http://example.com/badsize-alpha	ADD	BadSizeAlpha	abc	badsizeALPHAtoken000eeefff
EOF
sed -n '3,$p' "${FIXTURE}" >> "${INPUT4}"
make_index "${INPUT4}" "${INDEX4}"
expect_hit "${INDEX4}" "OpenAI2025" "http://example.com/doc1"
expect_hit "${INDEX4}" "abc123" "http://example.com/doc5"
expect_no_hit "${INDEX4}" "badsizeNEGtoken888aaabbb"
expect_no_hit "${INDEX4}" "badsizeOVERtoken999cccddd"
expect_no_hit "${INDEX4}" "badsizeALPHAtoken000eeefff"

case_begin "makeindex: default body size boundaries are handled"
INPUT5="${TMP_ROOT}/input_size_boundary_default.txt"
INDEX5="${TMP_ROOT}/index5"
python3 - "${FIXTURE}" "${INPUT5}" <<'PY'
import sys

src, dst = sys.argv[1], sys.argv[2]
size_map = {
    "http://example.com/doc1": "24",
    "http://example.com/doc2": "23",
    "https://example.com/doc3": "102400",
    "http://example.com/doc5": "102401",
}

with open(src, encoding="utf-8") as fin, open(dst, "w", encoding="utf-8") as fout:
    for line in fin:
        line = line.rstrip("\n")
        fields = line.split("\t")
        if len(fields) == 5 and fields[0] in size_map:
            fields[3] = size_map[fields[0]]
            line = "\t".join(fields)
        fout.write(line + "\n")
PY
make_index "${INPUT5}" "${INDEX5}"
expect_hit "${INDEX5}" "OpenAI2025" "http://example.com/doc1"
expect_hit "${INDEX5}" "サンプル本文" "https://example.com/doc3"
expect_no_hit "${INDEX5}" "検索用のテスト本文です"
expect_no_hit "${INDEX5}" "abc123"

exit 0
