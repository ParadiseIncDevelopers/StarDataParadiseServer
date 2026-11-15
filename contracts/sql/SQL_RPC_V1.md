# SQL CRUD extension for wire v1

The published v1 foundation archive remains immutable. A Server advertises this extension as the Boolean `sql_crud_v1` capability after TLS authentication when a store is initialized. The extension uses the v1 framing, typed JSON envelope, error codes and connection sequencing. This capability identifies the operation names and fields in the table below. Reserved foundation capabilities and operations continue to describe the original draft shapes.

| Request | Body | Success |
| --- | --- | --- |
| `query.prepare` | `sql`, optional `parameter_types` object keyed by parameter name | `statement_id` |
| `execute` | `statement_id`, `parameters` object keyed by name, `row_limit`, optional `request_token` | `affected_rows`, optional `lsn`, or `result_id` and ordered `columns` |
| `fetch` | `result_id`, `batch_rows` | `batch` with ordered rows and end flags. The first batch contains `columns`. |
| `statement_close` and `result_cancel` | `statement_id` or `result_id`, respectively | Empty body |
| `metadata` | Empty body | Ordered catalog `objects` |
| `tx_begin` and `tx_rollback` | Empty body | Empty body |
| `tx_commit` | `request_token` | `status: "committed"`, `lsn` |
| `tx_savepoint`, `tx_rollback_to`, `tx_release` | `name` | Empty body |
| `tx_outcome` | `request_token` | `status: "committed"` and `lsn`, or `status: "unknown"` |

The parameter type object is `{"type":"BIGINT"}` for an integer or `{"type":"DECIMAL","precision":18,"scale":4}` for an exact decimal. Other declaration names are BOOLEAN, TEXT, BLOB, DATE, TIME and TIMESTAMP WITH TIME ZONE. Values use the [v1 type mapping](../v1/TYPE_MAPPING.md). DATE and TIME use the text representation with the type established by metadata. Timestamp RFC 3339 offsets are preserved in the timestamp tuple. Named time zone IDs and composite types require later extensions.

Write execution in autocommit and explicit commit each require a fresh 32-character lowercase hexadecimal request token. Explicit transaction statements omit the token until commit. The authenticated actor and request token index a ledger row written in the same transaction as the edits. A duplicate write token is rejected. After a lost reply, query the outcome on a new authenticated session. An expired or absent outcome is unknown. The result is visible to the matched actor for 24 hours. Retention of the storage row and WAL continues through this etapa. SQL text and parameter values are never concatenated.

The session has at most one open result. Fetch is pull-driven and bounded by row count and memory. Cancelling a result releases its resources and an implicit read transaction. Disconnect discards open transaction edits and result state. The Server has one exclusive serializable transaction lease and caps its lifetime. Other sessions may retry a timed-out read once the lease is free. A local transport cancellation or lost response during a commit can leave its outcome unknown. This extension does not implement the reserved out-of-band cancel operation.

Schema binding is repeated in the executing transaction. Statements with parse/bind diagnostics carry the line and column in the wire error. Product and ABI versions are independent of this capability.
