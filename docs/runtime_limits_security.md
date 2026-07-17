# Runtime resource limits and write authentication

`yappod_front` and `yappod_core` load the same fail-closed runtime policy from the shared
application TOML passed with `--config PATH`. Invalid values prevent the daemon from starting.
Environment-variable runtime configuration is not supported.

```toml
[daemon]
max_inflight = 4
max_inflight_bytes = 4194304
request_timeout_ms = 5000
write_token = "replace-with-at-least-16-bytes"
```

| Key | Default | Allowed range | Meaning |
|---|---:|---:|---|
| `daemon.max_inflight` | `4` | 1..1024 | Maximum admitted v2 requests per process |
| `daemon.max_inflight_bytes` | `4194304` | 1..1073741824 | Maximum aggregate declared request bytes per process |
| `daemon.request_timeout_ms` | `5000` | 1..60000 | Receive/send deadline for client and front-to-core sockets |
| `daemon.write_token` | unset | 16..255 non-whitespace bytes | Bearer token required by `/v2/documents:batch` |

Requests exceeding either in-flight limit receive HTTP `503` with error code `overloaded`.
Socket deadlines bound slow request bodies, stalled front-to-core communication, and stalled
response writes. They do not preempt CPU work already executing inside a query.

## Write authentication

When `daemon.write_token` is configured, every update must carry the exact value as a Bearer token.
Missing or incorrect credentials receive HTTP `401`. Comparisons are constant-time with respect
to the configured and supplied token lengths. The front passes authenticated writes to the core
in a bounded authenticated envelope, and the core validates it again.

If the token is unset, writes remain enabled for trusted local deployments. Configure the token
before exposing the HTTP listener beyond a trusted host. Store secret-bearing application TOML
with owner-only permissions and do not commit it. The core port is an internal protocol endpoint
and must not be exposed to untrusted networks.

The writer lock described in [Compaction, GC, and crash recovery](compaction_recovery.md)
serializes update and compaction publication. Runtime admission limits are separate and protect
daemon workers and request memory before index mutation begins.
