#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
FIXTURE="${ROOT_DIR}/tests/fixtures/index.txt"
INDEX_DIR="$(mktemp -d)"
INDEX_DIR_LOOSE="$(mktemp -d)"
CURRENT_CASE="e2e"

# shellcheck source=tests/test_helpers.sh
source "${ROOT_DIR}/tests/test_helpers.sh"

cleanup() {
  rm -rf "${INDEX_DIR}"
  rm -rf "${INDEX_DIR_LOOSE}"
}
trap cleanup EXIT

on_error() {
  local rc=$?
  echo "[ERROR] case='${CURRENT_CASE}' rc=${rc} cmd='${BASH_COMMAND}'" >&2
  dump_sanitizer_logs
}
trap on_error ERR

mkdir -p "${INDEX_DIR}/pos"
mkdir -p "${INDEX_DIR_LOOSE}/pos"

"${BUILD_DIR}/yappo_makeindex" -f "${FIXTURE}" -d "${INDEX_DIR}" >/dev/null
"${BUILD_DIR}/yappo_makeindex" -f "${FIXTURE}" -d "${INDEX_DIR_LOOSE}" --min-body-size 1 >/dev/null

run_search() {
  "${BUILD_DIR}/search" -l "${INDEX_DIR}" "$@"
}

run_search_loose() {
  "${BUILD_DIR}/search" -l "${INDEX_DIR_LOOSE}" "$@"
}

expect_hit() {
  local query="$1"
  local expect="$2"
  if ! run_search "${query}" | grep -q "${expect}"; then
    echo "Expected hit '${expect}' for query: ${query}" >&2
    exit 1
  fi
}

expect_no_hit() {
  local query="$1"
  run_search "${query}" | grep -q "Hit num: 0\\|not found" || {
    echo "Expected no hit for query: ${query}" >&2
    exit 1
  }
}

expect_no_match_args() {
  local expect="$1"
  shift
  if run_search "$@" | grep -q "${expect}"; then
    echo "Expected no match '${expect}' for args: $*" >&2
    exit 1
  fi
}

expect_hit_args() {
  local expect="$1"
  shift
  if ! run_search "$@" | grep -q "${expect}"; then
    echo "Expected hit '${expect}' for args: $*" >&2
    exit 1
  fi
}

expect_hit_loose() {
  local query="$1"
  local expect="$2"
  if ! run_search_loose "${query}" | grep -q "${expect}"; then
    echo "Expected hit '${expect}' in loose index for query: ${query}" >&2
    exit 1
  fi
}

expect_hit_with_domain() {
  local query="$1"
  local url="$2"
  local domainid="$3"
  local out
  out="$(run_search "${query}")"
  if ! echo "${out}" | grep -q "URL:${url}"; then
    echo "Expected URL hit '${url}' for query: ${query}" >&2
    exit 1
  fi
  if ! echo "${out}" | grep -B2 "URL:${url}" | grep -q "(domainid:${domainid})"; then
    echo "Expected domainid '${domainid}' for URL '${url}' query: ${query}" >&2
    exit 1
  fi
}

expect_hit "テスト" "http://example.com/doc1"
expect_hit "テスト" "http://example.com/doc2"
expect_hit "OpenAI2025" "http://example.com/doc1"
expect_hit "openai2025" "http://example.com/doc1"
expect_hit "サンプル本文" "https://example.com/doc3"
expect_hit "abc123" "http://example.com/doc5"
expect_hit "foo+bar@example.com" "http://example.com/doc5"
expect_hit "日本語" "http://example.com/doc5"
expect_hit "本文です。" "http://example.com/doc1"
expect_hit "本文です。OpenAI2025" "http://example.com/doc1"
expect_hit "検索用のテスト本文です" "http://example.com/doc2"
expect_hit "日本語とabc123" "http://example.com/doc5"
expect_hit_with_domain "サンプル本文" "https://example.com/doc3" "1"
expect_no_hit "削除対象"
expect_no_hit "短い"
expect_no_hit "abc124"
expect_no_hit "OpenAI"
expect_no_hit "2025"
expect_no_hit "abc"
expect_no_hit "123"
expect_no_hit "テスト。"
expect_no_hit "本文です。OpenAI"
expect_no_hit "日本語とabc"
expect_hit_loose "短い" "http://example.com/skip"
expect_hit_args "http://example.com/doc2" -a "テスト" "本文" "検索"
expect_no_match_args "http://example.com/doc1" -a "テスト" "本文" "検索"
