#!/bin/sh
set -eu

example_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/yappod-wikipedia-e2e.XXXXXX")
started=0

cleanup() {
  result=$?
  if [ "$result" -ne 0 ] && [ -d "$tmp_dir/run" ]; then
    for log in "$tmp_dir"/run/*.error "$tmp_dir"/run/*.log; do
      if [ -f "$log" ]; then
        echo "--- $log" >&2
        tail -n 80 "$log" >&2
      fi
    done
  fi
  if [ "$started" -eq 1 ]; then
    YAPPOD_RUN_DIR="$tmp_dir/run" "$example_dir/scripts/stop_demo.sh" >/dev/null 2>&1 || true
  fi
  rm -rf "$tmp_dir"
}
trap cleanup 0 1 2 15

ports=$(python3 -c 'import socket; sockets=[socket.socket() for _ in range(4)]; [s.bind(("127.0.0.1",0)) for s in sockets]; print(*(s.getsockname()[1] for s in sockets)); [s.close() for s in sockets]')
set -- $ports
export YAPPOD_RUN_DIR="$tmp_dir/run"
export YAPPOD_CORE_PORT=$1
export YAPPOD_FRONT_PORT=$2
export YAPPOD_WEB_PORT=$3
export YAPPOD_MOCK_LLM_PORT=$4
export YAPPOD_WRITE_TOKEN="stage5-integration-token"
export YAPPOD_DEMO_MOCK_LLM=1
export WIKIPEDIA_E2E_WEB_URL="http://127.0.0.1:$YAPPOD_WEB_PORT"
export WIKIPEDIA_E2E_YAPPOD_URL="http://127.0.0.1:$YAPPOD_FRONT_PORT"

python3 "$example_dir/wikipedia_data.py" convert-dump \
  --input "$example_dir/tests/fixtures/wikiextractor.jsonl" \
  --output "$tmp_dir/documents.ndjson" \
  --limit 10 >/dev/null

"$example_dir/scripts/start_demo.sh" "$tmp_dir/documents.ndjson" "$tmp_dir/index"
started=1
if "$example_dir/scripts/start_demo.sh" "$tmp_dir/documents.ndjson" "$tmp_dir/index" >/dev/null 2>&1; then
  echo "duplicate demo start unexpectedly succeeded" >&2
  exit 1
fi
curl -fsS "$WIKIPEDIA_E2E_WEB_URL/api/status" >/dev/null
node "$example_dir/tests/e2e.mjs"
"$example_dir/scripts/stop_demo.sh"
started=0

for url in \
  "$WIKIPEDIA_E2E_WEB_URL/api/status" \
  "$WIKIPEDIA_E2E_YAPPOD_URL/health/live" \
  "http://127.0.0.1:$YAPPOD_MOCK_LLM_PORT/health"; do
  if curl -fsS "$url" >/dev/null 2>&1; then
    echo "service is still reachable after stop: $url" >&2
    exit 1
  fi
done

echo "Wikipedia search demo start/stop E2E passed"
