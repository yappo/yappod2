#!/bin/sh
set -eu

example_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
web_dir="$example_dir/web"
input=${1:-"$example_dir/data/documents.ndjson"}
index=${2:-"$example_dir/index"}
run_dir=${YAPPOD_RUN_DIR:-"$example_dir/run"}
core_port=${YAPPOD_CORE_PORT:-10086}
front_port=${YAPPOD_FRONT_PORT:-10080}
web_host=${YAPPOD_WEB_HOST:-127.0.0.1}
web_port=${YAPPOD_WEB_PORT:-4173}
mock_host=${YAPPOD_MOCK_LLM_HOST:-127.0.0.1}
mock_port=${YAPPOD_MOCK_LLM_PORT:-1234}
mock_enabled=${YAPPOD_DEMO_MOCK_LLM:-0}
cleanup_needed=0

cleanup() {
  if [ "$cleanup_needed" -eq 1 ]; then
    YAPPOD_RUN_DIR="$run_dir" "$example_dir/scripts/stop_demo.sh" >/dev/null 2>&1 || true
  fi
}
trap cleanup 0 1 2 15

for command in curl node npm; do
  if ! command -v "$command" >/dev/null 2>&1; then
    echo "required command not found: $command" >&2
    exit 1
  fi
done

if [ ! -x "$example_dir/../../build/yappo_makeindex" ] ||
   [ ! -x "$example_dir/../../build/yappod_core" ] ||
   [ ! -x "$example_dir/../../build/yappod_front" ]; then
  echo "yappod binaries not found; run cmake --build build -j at the repository root" >&2
  exit 1
fi

if [ ! -d "$web_dir/node_modules" ]; then
  echo "Web dependencies not found; run npm install in $web_dir" >&2
  exit 1
fi

if [ -n "${YAPPOD_WRITE_TOKEN:-}" ] && [ -n "${YAPPOD_V2_WRITE_TOKEN:-}" ] &&
   [ "$YAPPOD_WRITE_TOKEN" != "$YAPPOD_V2_WRITE_TOKEN" ]; then
  echo "YAPPOD_WRITE_TOKEN and YAPPOD_V2_WRITE_TOKEN must match" >&2
  exit 1
fi
if [ -z "${YAPPOD_WRITE_TOKEN:-}" ] && [ -n "${YAPPOD_V2_WRITE_TOKEN:-}" ]; then
  echo "set YAPPOD_WRITE_TOKEN so the BFF and daemon share the write token" >&2
  exit 1
fi
if [ -n "${YAPPOD_WRITE_TOKEN:-}" ]; then
  YAPPOD_V2_WRITE_TOKEN=$YAPPOD_WRITE_TOKEN
  export YAPPOD_V2_WRITE_TOKEN
fi

case "$mock_enabled" in
  0|1) ;;
  *) echo "YAPPOD_DEMO_MOCK_LLM must be 0 or 1" >&2; exit 1 ;;
esac

mkdir -p "$run_dir"
for file in core.pid front.pid web.pid mock-llm.pid; do
  if [ -f "$run_dir/$file" ]; then
    echo "PID file already exists: $run_dir/$file" >&2
    exit 1
  fi
done

if [ ! -f "$index/manifest.json" ]; then
  if [ -e "$index" ]; then
    echo "index path exists but is not a valid index: $index" >&2
    exit 1
  fi
  if [ ! -f "$input" ]; then
    echo "canonical NDJSON not found: $input" >&2
    exit 1
  fi
  "$example_dir/scripts/build_index.sh" "$input" "$index"
fi

YAPPOD_RUN_DIR="$run_dir" \
YAPPOD_CORE_PORT="$core_port" \
YAPPOD_FRONT_PORT="$front_port" \
  "$example_dir/scripts/start_daemons.sh" "$index"
cleanup_needed=1

if [ "$mock_enabled" -eq 1 ]; then
  (
    cd "$web_dir"
    MOCK_LLM_HOST="$mock_host" \
    MOCK_LLM_PORT="$mock_port" \
    MOCK_LLM_MODEL="yappod-demo-mock" \
      exec node scripts/mock-llm.mjs
  ) >"$run_dir/mock-llm.log" 2>"$run_dir/mock-llm.error" &
  echo "$!" >"$run_dir/mock-llm.pid"

  i=0
  while [ "$i" -lt 50 ]; do
    if curl -fsS "http://$mock_host:$mock_port/health" >/dev/null 2>&1; then
      break
    fi
    sleep 0.1
    i=$((i + 1))
  done
  if [ "$i" -eq 50 ]; then
    echo "mock LLM did not become ready; inspect $run_dir/mock-llm.error" >&2
    exit 1
  fi
  LLM_BASE_URL="http://$mock_host:$mock_port/v1"
  LLM_MODEL="yappod-demo-mock"
  export LLM_BASE_URL LLM_MODEL
fi

(cd "$web_dir" && npm run build)

(
  cd "$web_dir"
  NODE_ENV=production \
  HOST="$web_host" \
  PORT="$web_port" \
  YAPPOD_URL="http://127.0.0.1:$front_port" \
    exec node server/dist/index.js
) >"$run_dir/web.log" 2>"$run_dir/web.error" &
web_pid=$!
echo "$web_pid" >"$run_dir/web.pid"

i=0
status=
while [ "$i" -lt 50 ]; do
  if ! kill -0 "$web_pid" 2>/dev/null; then
    wait "$web_pid" 2>/dev/null || true
    echo "Web application exited during startup." >&2
    if [ -s "$run_dir/web.error" ]; then
      sed -n '1,20p' "$run_dir/web.error" >&2
    else
      echo "No error output was produced; inspect $run_dir/web.log" >&2
    fi
    exit 1
  fi
  status=$(curl -fsS "http://$web_host:$web_port/api/status" 2>/dev/null || true)
  if grep -q '^Wikipedia search web is ready:' "$run_dir/web.log" 2>/dev/null &&
     printf '%s' "$status" | grep -q '"ready":true'; then
    break
  fi
  sleep 0.1
  i=$((i + 1))
done
if [ "$i" -eq 50 ]; then
  echo "Web application did not become ready at http://$web_host:$web_port/api/status" >&2
  if [ -n "$status" ]; then
    echo "Last status response: $status" >&2
  fi
  if [ -s "$run_dir/web.error" ]; then
    sed -n '1,20p' "$run_dir/web.error" >&2
  else
    echo "No error output was produced; inspect $run_dir/web.log" >&2
  fi
  exit 1
fi

cleanup_needed=0
trap - 0 1 2 15
echo "Wikipedia search demo is ready: http://$web_host:$web_port"
if [ "$mock_enabled" -eq 1 ]; then
  echo "mock LLM is enabled for local testing"
fi
