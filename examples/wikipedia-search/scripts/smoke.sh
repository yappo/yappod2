#!/bin/sh
set -eu

front_port=${YAPPOD_FRONT_PORT:-18400}
query=${1:-日本}
base_url="http://127.0.0.1:$front_port"

printf '%s\n' '--- lexical search ---'
curl -fsS \
  -H 'Content-Type: application/json' \
  --data "{\"query\":$(printf '%s' "$query" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read(), ensure_ascii=False))'),\"mode\":\"lexical\",\"scope\":\"documents\",\"limit\":5}" \
  "$base_url/v2/search"
printf '\n%s\n' '--- RAG retrieval ---'
curl -fsS \
  -H 'Content-Type: application/json' \
  --data "{\"query\":$(printf '%s' "$query" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read(), ensure_ascii=False))'),\"mode\":\"lexical\",\"limit\":5,\"max_passages_per_document\":2,\"max_context_bytes\":16384}" \
  "$base_url/v2/retrieve"
printf '\n'
