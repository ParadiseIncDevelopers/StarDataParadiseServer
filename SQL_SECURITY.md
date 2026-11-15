# SQL security and sessions

An initialized Server advertises sql_security_v1 on the authenticated TLS connection. The client connection binds to the verified account identity. Its privileges are read from the current Server-side user and role catalog on every prepare, execution, fetch and management request that accesses protected state.

## Users and roles

The first security-catalog commit records the configured bootstrap administrator and the requested change in the WAL. Account and role IDs survive restart. CREATE USER creates a disabled account. Set its password using the authenticated password_set operation. The administrator may reset a password. Other users may change their own password after proving the old one. Passwords use the same salted PBKDF2-SHA256 implementation as the bootstrap configuration. A successful password change activates the durable verifier.

```sql
CREATE USER analyst;
CREATE ROLE catalog_readers;
GRANT SELECT ON TABLE main.public.inventory TO ROLE catalog_readers;
GRANT catalog_readers TO USER analyst;
REVOKE SELECT ON TABLE main.public.inventory FROM ROLE catalog_readers;
```

Grants may target a database, schema, table, view, routine or global scope. SELECT authorization includes the base table of an invoker-rights view. DML checks the current table privilege. Catalog mutations check global DDL permission and user/role changes require the administrator. Catalog metadata is filtered by the current object rights and denied when no objects are visible. User, role and credential rows use reserved tags separate from externally visible SQL objects.

GRANT, REVOKE and password updates are autocommit operations. The transaction gate serializes writes. A privilege snapshot becomes visible on durable publication, so a revoke takes effect on existing sessions after commit. An uncertain security commit pauses the security snapshot until restart. Commit outcomes are scoped to the authenticated actor and become unknown after the ledger retention window.

## Session control

| Operation | Effect |
| --- | --- |
| session_reset | Releases prepared statements, results and pending transaction state. The authenticated actor stays bound to the TLS connection. |
| session_list | Requires monitor permission and shows authenticated sessions, active requests, running time, transaction state and current lease wait time. |
| query_cancel | Requests cancellation of a specific active request. The requester needs control permission or the same actor on a separate connection. The query returns its own final status. |
| session_terminate | Cancels active work and closes the target connection under the same ownership and control rules. |
| password_set | Changes a credential for the authorized identity and accepts a request token for the durable commit ledger. |

The IO thread handles control requests while a bounded worker pool executes SQL. Work loops and transaction waits check the cancellation token. The Server caps transaction lease age and releases private changes when a lease expires or a disconnected session is destroyed. Expiry is checked before storage work. An operation already in a synchronous Windows flush reaches the next boundary first.

The session list reports observed Server state. A client pool can reset the connection before reuse. The default work context uses main.public and the actor's live membership. SQL statements carry no selectable role override or temporary schema at this stage.
