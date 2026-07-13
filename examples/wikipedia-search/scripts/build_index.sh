#!/bin/sh
set -eu

example_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
repo_root=$(CDPATH= cd -- "$example_dir/../.." && pwd)
input=${1:-"$example_dir/data/documents.ndjson"}
index=${2:-"$example_dir/index"}

if [ -e "$index" ]; then
  echo "index path already exists: $index" >&2
  echo "Choose a new path or remove the old sample index explicitly." >&2
  exit 1
fi

"$repo_root/build/yappo_makeindex" build \
  --config "$example_dir/config.toml" \
  --input "$input" \
  --index "$index"
