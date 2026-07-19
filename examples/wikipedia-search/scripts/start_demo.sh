#!/bin/sh
set -eu
example_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
web_dir=$(CDPATH= cd -- "$example_dir/../search-web" && pwd)
exec node "$web_dir/scripts/stack.mjs" start "$@"
