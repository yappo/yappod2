#!/bin/sh
set -eu

web_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
repo_root=$(CDPATH= cd -- "$web_dir/../.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/yappod-search-web-e2e.XXXXXX")
config="$tmp_dir/application.toml"
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
    "$web_dir/scripts/stop.sh" --config "$config" >/dev/null 2>&1 || true
  fi
  rm -rf "$tmp_dir"
}
trap cleanup 0 1 2 15

ports=$(python3 -c 'import socket; sockets=[socket.socket() for _ in range(4)]; [s.bind(("127.0.0.1",0)) for s in sockets]; print(*(s.getsockname()[1] for s in sockets)); [s.close() for s in sockets]')
set -- $ports
core_port=$1
front_port=$2
web_port=$3
mock_port=$4

cat >"$config" <<EOF
schema_version = 1

[build]
yappo_makeindex = "$repo_root/build/yappo_makeindex"
input = "$web_dir/tests/fixtures/documents.vector.ndjson"
index_config = "$web_dir/tests/fixtures/config.vector.toml"
index_directory = "$tmp_dir/index"

[daemon]
run_directory = "$tmp_dir/run"
core_host = "127.0.0.1"
core_port = $core_port
front_host = "127.0.0.1"
front_port = $front_port
max_inflight = 4
max_inflight_bytes = 4194304
request_timeout_ms = 5000
write_token = "stage5-integration-token"

[web]
host = "127.0.0.1"
port = $web_port
yappod_timeout_ms = 5000

[llm]
base_url = "http://127.0.0.1:$mock_port/v1"
model = "yappod-demo-mock"
effort = "low"
timeout_ms = 30000

[embedding]
provider = "lmstudio"
base_url = "http://127.0.0.1:$mock_port/v1"
model = "yappod-demo-mock"
model_id = "yappod-demo-mock"
dimensions = 3
prompt_profile = "plain"
timeout_ms = 60000
batch_size = 16

[mock]
enabled = true
host = "127.0.0.1"
port = $mock_port
model = "yappod-demo-mock"
answer = "参照資料から確認できる内容です。[1]"
embedding_dimensions = 3
EOF

node "$web_dir/scripts/stack.mjs" build --config "$config"
"$web_dir/scripts/start.sh" --config "$config"
started=1
node "$web_dir/tests/e2e.mjs" --config "$config"
"$web_dir/scripts/stop.sh" --config "$config"
started=0

for url in \
  "http://127.0.0.1:$web_port/api/status" \
  "http://127.0.0.1:$front_port/health/live" \
  "http://127.0.0.1:$mock_port/health"; do
  if curl -fsS "$url" >/dev/null 2>&1; then
    echo "service is still reachable after stop: $url" >&2
    exit 1
  fi
done

echo "search Web stack E2E passed"
