# RFC: Atomic SQL Chain / Transactional Job Enqueue

**Status:** PROPOSED — NOT IMPLEMENTED / NOT TESTED  
**Date:** 2026-11-20  
**Target:** MySQL 26.10.0-based StarDataParadise Server fork; `sdp_compatibility_mode=legacy`  
**Related acceptance plan:** [ATOMIC_CHAIN_ACCEPTANCE.md](../../compatibility/tests/ATOMIC_CHAIN_ACCEPTANCE.md)

## 1. Goal

Provide one prepared SQL statement that:
1. mutates one transactional row,
2. passes its **actual resulting column value** to a server-managed durable job,
3. stores the DML change, History/Decision effects where enabled, queue row, and idempotency receipt in **the same InnoDB transaction**,
4. returns the new or previously committed `job_id`.

No developer-written trigger, stored procedure, or client-side second command is required. An **asynchronous worker failure after commit does not roll back a committed DML change**; retry/compensation is a separate policy.

## 2. Observed baseline

- The original C++ engine provides serializable transactions, WAL, statement savepoints and `job_submit` in an existing transaction (`TRANSACTIONS_AND_WAL.md`, `JOBS.md`, `sql/session.cpp`, `jobs/manager.cpp`). Its parser explicitly rejects DML `RETURNING` in `sql/syntax.cpp`.
- The current MySQL-based fork has `sdp_jobs.compute_jobs` (InnoDB) and a compute worker (`compatibility/jobs/compute_jobs.py`). Queue entries already have `job_id`, `owner`, `request_token`, `iterations` and state. This is **not** a built-in shipment/delivery worker.
- The native MySQL fork tracks its parser changes in `compatibility/upstream/server-26.10.0.patch`. The legacy wire bridge must not be mistaken for a native SQL implementation. No existing generic one-statement DML-to-job chain was found.
- Migration is in progress: do not add a legacy-engine-only implementation and advertise it as production MySQL capability.

## 3. Proposed vendor SQL syntax (v1)

```sql
SET SESSION sdp_compatibility_mode = 'legacy';

UPDATE orders
SET status = 'approved'
WHERE id = 42 AND status = 'pending'
THEN ENQUEUE JOB compute
     WITH (count = RESULT.iterations)
     REQUEST '0123456789abcdef0123456789abcdef'
RETURNING JOB_ID;
```

Here `RESULT.iterations` is read from the final changed row, **not** from a second SELECT or a client-provided duplicate value. For an actual logistics deployment, `shipment` and `order_id` will require an installed, authorized **shipment job kind**, which is not currently available:

```sql
-- FUTURE example; NOT EXECUTABLE in the current Server
UPDATE orders SET status = 'approved' WHERE id = 42 AND status = 'pending'
THEN ENQUEUE JOB shipment WITH (order_id = RESULT.id)
     REQUEST 'fedcba9876543210fedcba9876543210'
RETURNING JOB_ID;
```

The `REQUEST` token is a required, new 32-character lowercase hexadecimal idempotency identifier for each logical operation. Clients must reuse the same token on uncertain-outcome retry and use a new one for a new operation. Parameterized requests should be supported; SQL and parameter values must never be concatenated.

**v1 restrictions:**
- Only `UPDATE` of **exactly one matched row**, a transactional InnoDB table, and one `ENQUEUE JOB` clause. Zero or multiple matched rows fail the entire statement. The first runnable job kind is `compute`; its `count` accepts a BIGINT in `[1, 1000000000]`. No arbitrary code execution or remote webhook worker.
- `RESULT.column` refers to a typed column value from the final updated row after synchronous constraints and Decision checks. Require known column names and a type-compatible job argument. No SQL expression interpolation into worker commands.
- One result row with `JOB_ID` (text identifier); clients can use the ordinary MySQL result-set API. `RETURNING` without `THEN ENQUEUE`, multi-row output, `INSERT`, `DELETE` and multiple chained steps are separate later phases.
- Do not advertise support until the native parser, executor, permissions, crash recovery and end-to-end tests pass on the fork.

## 4. Required transaction semantics

1. Parse and bind the DML, result-row reference, job kind, argument names, argument types and token before changing any data.
2. Authorize the DML on the source table and `ENQUEUE` on the selected job kind (separate capability, not general write access to `sdp_jobs`). Audit actor and request identity; do not trust user-supplied owner names.
3. At the **start of the transaction**, serialize on an `(owner, request_token)` chain receipt. On duplicate token and matching immutable request fingerprint, return the previously committed `job_id` **without running DML again**. Different fingerprint must fail. Do not rely only on queue `UNIQUE(owner, request_token)`: retry must not re-run UPDATE/Decision/triggers.
4. Execute one-row UPDATE through the normal SQL executor. Capture its final updated row image from that execution path and evaluate `RESULT.column` from it. A second table read is not an equivalent capture mechanism.
5. Validate job capabilities and bounded arguments. Insert the durable job row and receipt through the **same InnoDB transaction**, under the same authenticated actor. Do not invoke the worker or external network I/O before commit.
6. If DML, permission checks, row-cardinality checks, queue insert, capacity, or receipt insertion fails, roll back the entire **statement** (including any row changes and queue data). In an explicit user transaction, preserve earlier successful statements; `ROLLBACK` also removes the staged job.
7. Autocommit publishes the row update and queued work together. Under an explicit transaction, both remain private until `COMMIT`. Workers only claim committed queue entries. Unknown commit outcomes must be queried by request token instead of blindly repeating DML.
8. The History journal/audit integration must not claim atomicity until tested against the MySQL implementation. No cross-resource atomicity promise for non-InnoDB tables, external triggers or external transport.

**Receipt proposal:** an internal transactional `sdp_jobs.chain_receipts` table with `(owner, request_token)` unique, a canonical request fingerprint (no plaintext parameter secrets), `job_id` foreign key, and durable outcome. Enforce least privilege so users cannot forge receipts or enqueue other users' jobs. Only native Server code or privileged internal implementation can write these tables.

## 5. Error identifiers (proposed, not assigned runtime codes)

| Proposed symbolic identifier | Condition |
| --- | --- |
| `SDP_CHAIN_SYNTAX_41001` | Invalid chain SQL/unsupported composition |
| `SDP_CHAIN_CARDINALITY_41002` | UPDATE did not match exactly one row |
| `SDP_CHAIN_QUEUE_41003` | Queue unavailable/full; statement must roll back |
| `SDP_CHAIN_JOB_KIND_41004` | Uninstalled/unsupported job kind or arguments |
| `SDP_CHAIN_PERMISSION_41005` | Missing table or enqueue permission |
| `SDP_CHAIN_TOKEN_CONFLICT_41006` | Token reused with different request |
| `SDP_CHAIN_COMMIT_UNKNOWN_41007` | Transport/commit uncertainty; inspect receipt |

Wire and MySQL numeric errors/SQLSTATE mapping are **to be allocated** against existing error registries, not invented as shipped codes.

## 6. Implementation slices

- **A. Contract + acceptance:** this RFC and test cases; `NOT_RUN`, with the feature flag absent.
- **B. Native parser/binder:** extend MySQL fork `sql_yacc.yy` and SQL command handling in the tracked upstream patch. Bind an UPDATE AST, `RESULT.column`, literal/placeholder job parameters and request token. Keep the existing MySQL dialect unchanged unless legacy mode enabled.
- **C. Native executor + queue registry:** capture row, enforce one-row match, stage InnoDB queue record under the same THD transaction and support `compute`. Add chain receipt and full authorization. Do not implement this in Workbench-only preprocessing.
- **D. Integration:** publish `atomic_chain_v1` only after success; enable Workbench, CLI and SDK feature discovery, typed `JOB_ID` results, native and wire acceptance.
- **E. Later:** registered `shipment` worker and payload schema, `INSERT/DELETE`, guarded multi-row `FOR EACH ROW`, multiple chain stages, retry/dead-letter/compensation policy.

## 7. Release gate

NOT_RUN until separately proven: atomic success, failed enqueue rollback, explicit transaction rollback/commit, zero/multi-row rejection, authorization, token conflict/replay, crash-at-commit, worker isolation/visibility, WAL/History/Decision regression and Windows x64 Release/ASan. Keep existing capabilities unchanged meanwhile. See acceptance plan.
