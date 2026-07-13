# Runtime resource limits and write authentication

`yappod_front` and `yappod_core` load the same fail-closed runtime policy at startup. Invalid
values prevent the daemon from starting. Limits are configured with environment variables:

| Variable | Default | Allowed range | Meaning |
|---|---:|---:|---|
| `YAPPOD_V2_MAX_INFLIGHT` | `4` | 1..1024 | Maximum admitted v2 requests per process |
| `YAPPOD_V2_MAX_INFLIGHT_BYTES` | `4194304` | 1..1073741824 | Maximum aggregate declared request bytes per process |
| `YAPPOD_V2_REQUEST_TIMEOUT_MS` | `5000` | 1..60000 | Receive/send deadline for client and front-to-core sockets |
| `YAPPOD_V2_WRITE_TOKEN` | unset | 16..255 non-whitespace bytes | Bearer token required by `/v2/documents:batch` |

Requests exceeding either in-flight limit receive HTTP `503` with error code `overloaded`.
Socket deadlines bound slow request bodies, stalled front-to-core communication, and stalled
response writes. They do not preempt CPU work already executing inside a query.

## Write authentication

When `YAPPOD_V2_WRITE_TOKEN` is configured, every update must carry the exact value:

```sh
curl -H 'Content-Type: application/json' \
  -H "Authorization: Bearer $YAPPOD_V2_WRITE_TOKEN" \
  --data-binary @operations.json \
  http://127.0.0.1:8080/v2/documents:batch
```

Missing or incorrect credentials receive HTTP `401`. Comparisons are constant-time with respect
to the configured and supplied token lengths. The front passes authenticated writes to the core
in a bounded authenticated envelope, and the core validates it again; a client cannot bypass the
front by sending ordinary ingest JSON directly to the core while the token policy is enabled.

If the token is unset, writes remain enabled for backward-compatible local deployments. Configure
the token before exposing the HTTP listener beyond a trusted host. The core port is an internal
protocol endpoint and must not be exposed to untrusted networks.

The writer lock described in [Compaction, GC, and crash recovery](compaction_recovery.md)
serializes update and compaction publication. Runtime admission limits are intentionally separate:
they protect daemon workers and request memory before index mutation begins.
