# Atomic SQL Chain — acceptance plan (NOT_RUN)

This file is a **test specification**, not an executable test or proof of support. A native MySQL fork implementation is required before tests below can pass. Use a fresh, isolated InnoDB instance, a dedicated non-production schema, and a registered compute queue. Never run destructive fixtures on a user instance.

## Fixture

```sql
CREATE TABLE chain_orders (
    id BIGINT NOT NULL PRIMARY KEY,
    iterations BIGINT NOT NULL,
    status VARCHAR(20) NOT NULL
) ENGINE=InnoDB;
INSERT INTO chain_orders VALUES (42, 1000, 'pending'), (43, 500, 'pending');
SET SESSION sdp_compatibility_mode = 'legacy';
```

**Positive statement (future syntax — currently unsupported):**

```sql
UPDATE chain_orders
SET status = 'approved'
WHERE id = 42 AND status = 'pending'
THEN ENQUEUE JOB compute
     WITH (count = RESULT.iterations)
     REQUEST '0123456789abcdef0123456789abcdef'
RETURNING JOB_ID;
```

### Acceptance matrix

| Case | Setup/action | Required observable result |
| --- | --- | --- |
| A01 happy path | Execute positive statement | Exactly one order approved; exactly one queued job with `iterations=1000`; one returned JOB_ID; one receipt |
| A02 idem replay | Repeat identical statement and token after commit | Same JOB_ID; no repeated UPDATE/trigger/History event or second job; even if predicate now matches zero rows |
| A03 token collision | Reuse token for a different order/count | Conflict; no DML or extra job |
| A04 no matches | New token, predicate matches zero | Cardinality error; zero job rows and receipts |
| A05 many matches | New token, predicate matches 2 rows | Cardinality error; neither row changed and no queued job |
| A06 queue fails | Disable queue or exhaust capacity before statement | UPDATE and any same-statement History/audit rolled back; no job or receipt |
| A07 permission | Actor has UPDATE but not job ENQUEUE permission | Permission error; no changed order |
| A08 wrong type | Supply TEXT / NULL / out-of-range count | Bind/type/limit error; order and queue unchanged |
| A09 explicit rollback | BEGIN; chain statement; ROLLBACK | Neither order change, job nor receipt visible to a second connection |
| A10 explicit commit | BEGIN; chain statement; COMMIT | Both row and queue/receipt visible together after commit |
| A11 pending worker | Hold explicit transaction open while worker polls | Worker sees no staged job before commit |
| A12 worker failure | Complete chain commit, then fail worker | Order stays approved; job failure/retry recorded separately |
| A13 commit uncertainty | Disconnect/kill at commit boundary; retry same token | Receipt returns committed JOB_ID or confirms no commit; never double-writes |
| A14 concurrent replay | Two clients send same token simultaneously | Exactly one effect and one job; both compatible replies or a clear retryable conflict |
| A15 nontransactional source | Target MyISAM or nontransactional table | Fail closed before any mutation |
| A16 Decision/History | Apply rule, check WAL/History in supported mode | Rule denial rolls back queue, success makes row and queue visible together |
| A17 SQL dialect | Try vendor syntax in plain mysql compatibility mode | Explicit unsupported syntax/capability, no silent rewriting |
| A18 cap discovery | Query capabilities on native server, SDK and CLI | `atomic_chain_v1` false/absent until A01–A17 pass |

Use a **new** request token for every case except replay/conflict tests. Capture server version/commit, fork patch SHA, SQL mode, exact SQL, exit code, SQLSTATE, table snapshots, worker events and timestamps. Do not mark a check `PASS` without an actual test run; CMake/CI should only include runnable tests after B/C implementation.
