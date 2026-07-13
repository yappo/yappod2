#!/bin/sh
set -eu

example_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
repo_root=$(CDPATH= cd -- "$example_dir/../.." && pwd)
index=${1:-"$example_dir/index"}
core_port=${YAPPOD_CORE_PORT:-10086}
front_port=${YAPPOD_FRONT_PORT:-10080}
run_dir=${YAPPOD_RUN_DIR:-"$example_dir/run"}

if [ ! -f "$index/manifest.json" ]; then
  echo "valid index not found: $index" >&2
  exit 1
fi
index=$(CDPATH= cd -- "$index" && pwd)

mkdir -p "$run_dir"
if [ -f "$run_dir/core.pid" ] || [ -f "$run_dir/front.pid" ]; then
  echo "daemon PID file already exists in $run_dir" >&2
  exit 1
fi

cd "$run_dir"
"$repo_root/build/yappod_core" --index "$index" --port "$core_port"

cleanup_core() {
  if [ -f core.pid ]; then
    pid=$(sed -n '1p' core.pid)
    case "$pid" in *[!0-9]*|'') ;; *) kill "$pid" 2>/dev/null || true ;; esac
  fi
}
trap cleanup_core 0 1 2 15

i=0
while [ ! -f core.pid ] && [ "$i" -lt 50 ]; do
  sleep 0.1
  i=$((i + 1))
done
if [ ! -f core.pid ]; then
  echo "yappod_core did not start; inspect $run_dir/core.error" >&2
  exit 1
fi

"$repo_root/build/yappod_front" \
  --index "$index" \
  --core-host 127.0.0.1 \
  --core-port "$core_port" \
  --port "$front_port"

i=0
while [ "$i" -lt 50 ]; do
  if curl -fsS "http://127.0.0.1:$front_port/health/ready" >/dev/null 2>&1; then
    trap - 0 1 2 15
    echo "yappod is ready: http://127.0.0.1:$front_port"
    exit 0
  fi
  sleep 0.1
  i=$((i + 1))
done

echo "yappod_front did not become ready; inspect $run_dir/front.error" >&2
exit 1
