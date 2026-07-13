#!/bin/sh
set -eu

example_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
run_dir=${YAPPOD_RUN_DIR:-"$example_dir/run"}

pid_running() {
  pid=$1
  if ! kill -0 "$pid" 2>/dev/null; then
    return 1
  fi
  if [ -r "/proc/$pid/stat" ]; then
    state=$(sed -n 's/^.*) \([A-Z]\) .*$/\1/p' "/proc/$pid/stat")
    if [ "$state" = "Z" ]; then
      return 1
    fi
  fi
  return 0
}

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
    while pid_running "$pid" && [ "$i" -lt 50 ]; do
      sleep 0.1
      i=$((i + 1))
    done
  fi
  if pid_running "$pid"; then
    echo "$name did not stop after SIGTERM; sending SIGKILL to PID $pid" >&2
    if ! kill -KILL "$pid" 2>/dev/null; then
      return 1
    fi
    i=0
    while pid_running "$pid" && [ "$i" -lt 10 ]; do
      sleep 0.1
      i=$((i + 1))
    done
  fi
  if pid_running "$pid"; then
    echo "$name did not stop after SIGKILL: PID $pid" >&2
    return 1
  fi
  rm -f "$file"
}

stop_pid_file front
stop_pid_file core
echo "yappod daemons stopped"
