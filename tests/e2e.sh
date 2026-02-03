#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
FIXTURE="${ROOT_DIR}/tests/fixtures/index.txt"
INDEX_DIR="$(mktemp -d)"

cleanup() {
  rm -rf "${INDEX_DIR}"
}
trap cleanup EXIT

mkdir -p "${INDEX_DIR}/pos"

"${BUILD_DIR}/yappo_makeindex" -f "${FIXTURE}" -d "${INDEX_DIR}" >/dev/null

run_search() {
  "${BUILD_DIR}/search" -l "${INDEX_DIR}" "$@"
}

expect_hit() {
  local query="$1"
  local expect="$2"
  run_search "${query}" | grep -q "${expect}"
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
  run_search "$@" | grep -q "${expect}"
}

expect_hit "テスト" "http://example.com/doc1"
expect_hit "テスト" "http://example.com/doc2"
expect_hit "OpenAI2025" "http://example.com/doc1"
expect_hit "openai2025" "http://example.com/doc1"
expect_hit "example.com/doc3" "http://example.com/doc3"
expect_hit "abc123" "http://example.com/doc5"
expect_hit "foo+bar@example.com" "http://example.com/doc5"
expect_hit "日本語" "http://example.com/doc5"
expect_hit "本文です。" "http://example.com/doc1"
expect_hit "本文です。OpenAI2025" "http://example.com/doc1"
expect_hit "検索用のテスト本文です" "http://example.com/doc2"
expect_hit "日本語とabc123" "http://example.com/doc5"
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
expect_hit_args "http://example.com/doc2" -a "テスト" "本文" "検索"
expect_no_match_args "http://example.com/doc1" -a "テスト" "本文" "検索"
