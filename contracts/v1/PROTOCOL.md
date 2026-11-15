# StarDataParadise wire protocol v1

## Transport

V1 uses one authenticated TLS stream per session. Each frame starts with a 4-byte unsigned big-endian payload length followed by exactly that many bytes of UTF-8 JSON. Frame payloads range from 1 to 1,048,576 bytes. Zero, oversized, truncated and invalid frames terminate the connection. Receivers check the length before allocating the payload. Binary data appears as typed JSON values. There is no wire struct layout.

JSON uses RFC 8259 with a top-level object, no byte-order mark, unique object keys, a maximum nesting depth of 32 containers and valid UTF-8 scalar values. Unknown fields are ignored when their meaning does not affect the operation. Integers used by the envelope are JSON integers, with no fractional or exponent notation. A v1 receiver rejects every other major version. Future compatible additions use new optional fields or capability names. Changing framing, a required field or its meaning requires a new major version.

## Envelope

Request example

```json
{"v":1,"id":1,"op":"hello","body":{"client":"example"}}
```

Success example

```json
{"v":1,"id":1,"ok":true,"body":{"session_id":"a8512e717501f6afc73cc6d1b3b04cf9","product_version":"0.1.0","protocol_version":1}}
```

A failure has `v`, `id`, `ok:false` and an `error` object described in [ERROR_CODES.md](ERROR_CODES.md). `id` is an unsigned 32-bit integer increasing strictly from 1 within the connection. The server replies with the same ID. ID 0 is reserved for errors where no request ID can be safely recovered. Session, transaction, job and statement IDs are opaque server-assigned strings scoped to the authenticated identity. Use the error correlation ID for log correlation. Reconnect before a request ID would wrap.

## Operations

| Operation | Request body | Successful response body |
| --- | --- | --- |
| `hello` | Optional client name `client` | `session_id`, `product_version`, `protocol_version` |
| `auth` | `username`, `password` | `authenticated:true` |
| `capabilities` | Empty object | `features` object keyed by capability name with boolean values |
| `health` | Empty object | `status` and `product_version` |
| `metadata.list` | `kind`, optional `schema`, optional `cursor` | `entries` and nullable `next_cursor` |
| `query.prepare` | `sql` | `statement_id`, `parameters` |
| `query.execute` | `statement_id`, ordered typed `parameters`, optional `transaction_id` | `execution_id`, first `batch` if available |
| `query.fetch` | `execution_id`, `max_rows` | `batch` |
| `query.close` | `execution_id` or `statement_id` | Empty object |
| `transaction.begin` | Requested `isolation`, `read_only` | `transaction_id` and effective `isolation` |
| `transaction.commit`, `transaction.rollback`, `transaction.status` | `transaction_id` | `status`: active, committed, aborted or unknown |
| `cancel` | `session_id`, `request_id` | `accepted`; final outcome comes from the operation status |
| `job.submit` | `kind`, `options` | `job_id` |
| `job.status`, `job.cancel` | `job_id` | `status`, optional `result` or `error` |
| `admin.call` | `action`, `arguments` | `result` |

`hello` comes first and `auth` follows. Application operations require successful authentication. The v1 foundation implements `hello`, `auth`, `capabilities` and `health`. It returns `FEATURE_UNSUPPORTED` for unimplemented calls. The initial features are `tls`, `auth`, `health`, `metadata`, `sql`, `transactions`, `server_cancel`, `jobs`, `admin`, `history`, `decision` and `ml`. Unknown and absent capabilities default to false. The contract package includes a capability fixture with every feature false for a pre-service implementation.

Requests and responses are sequential on one connection with one outstanding request. The server reads no subsequent request until it has sent the current response. Fetch uses client pull and each batch fits in a frame. A separate authenticated control connection is reserved for a future server-side cancel implementation. Client-side cancellation can end a transport operation and does not assert that a server-side operation stopped.

Product release, protocol, storage, WAL, History, PL/SQL IR, model and installer package format each have independent versions. A missing format or feature stays unavailable. The initial product release is 0.1.0 and the protocol major is 1.
