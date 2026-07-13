#!/bin/sh
set -eu

example_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
run_dir=${YAPPOD_RUN_DIR:-"$example_dir/run"}

stop_pid_file() {
  name=$1
  file="$run_dir/$name.pid"
  if [ ! -f "$file" ]; then
    return
  fi
  pid=$(sed -n '1p' "$file")
  case "$pid" in
    *[!0-9]*|'') echo "invalid PID file: $file" >&2; return 1 ;;
  esac
  if kill "$pid" 2>/dev/null; then
    i=0
    while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 50 ]; do
      sleep 0.1
      i=$((i + 1))
    done
  fi
  if kill -0 "$pid" 2>/dev/null; then
    echo "$name did not stop: PID $pid" >&2
    return 1
  fi
  rm -f "$file"
}

stop_pid_file front
stop_pid_file core
echo "yappod daemons stopped"
