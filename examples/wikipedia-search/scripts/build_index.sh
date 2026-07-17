#!/bin/sh
set -eu

example_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
web_dir=$(CDPATH= cd -- "$example_dir/../search-web" && pwd)
if [ "${1:-}" = "--config" ] && [ "$#" -eq 2 ]; then
  exec node "$web_dir/scripts/stack.mjs" build --config "$2"
fi
echo "Usage: $0 --config PATH" >&2
exit 1
