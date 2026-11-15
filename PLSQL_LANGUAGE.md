# PL/SQL language subset

Etap 16 supports stored functions and procedures, anonymous DECLARE/BEGIN/END blocks, scalar typed locals, IN/OUT/IN OUT parameters and CREATE OR REPLACE. Functions are called with a standalone SELECT function call. Procedures use CALL and return OUT and IN OUT parameters as typed columns in declaration order. An OUT argument can be omitted or represented by NULL when the full signature is supplied.

```sql
CREATE FUNCTION stock_for(p_id IN BIGINT) RETURN BIGINT AS
BEGIN
    RETURN (SELECT quantity FROM stock AS s WHERE s.id = :p_id);
END;
```

In procedural expressions, an unqualified name resolves in the routine's lexical frame. Within an embedded SELECT, `:name` binds a typed lexical value and ordinary identifiers resolve as SQL columns. Embedded scalar SELECT reads from the caller's transaction. Multiple returned rows raise an error and zero returned rows produce NULL. EXISTS produces a Boolean. Routines use invoker rights and the caller keeps control of transaction completion.

The subset includes assignments, IF/ELSIF/ELSE, searched CASE, inclusive FOR, WHILE, LOOP, EXIT and CONTINUE with optional WHEN, RETURN, RAISE and EXCEPTION handlers. BIGINT arithmetic checks overflow and division by zero. Decimal literals, comparison and conversion preserve exact scale. The runtime limits execution to 100,000 steps, depth 32, 1 MiB of frame memory and two seconds per invocation tree. Query deadlines and cancellation also apply to SQL host calls. Routine execution can read SQL and manipulate local values. Record, collection, cursor and trigger support follows in the next etap.

CREATE OR REPLACE advances a routine revision while preserving its object ID. A routine whose referenced table or routine version changed requires recompilation. Use CREATE OR REPLACE to bind its source against the current schema.

## Cursors, compound values and triggers

Etap 17 adds SELECT INTO, OPEN cursor FOR SELECT, FETCH cursor INTO, CLOSE and EMIT cursor AS name. A local cursor is declared as REFCURSOR. An OUT REFCURSOR produces a named result set using the parameter name. SELECT INTO requires exactly one row. No rows raises no_data_found and several rows raises too_many_rows. FETCH at end leaves the target variables unchanged. Cursor results carry a row count and ordered typed column metadata. OUT and IN OUT scalar values appear first in a result set named out. Additional sets follow EMIT order, then OUT cursor declaration order.

RECORD and COLLECTION are bounded routine parameter and variable types. A record carries ordered named fields and a collection carries ordered items with their individual protocol tags. NULL, exact decimal scale and binary values retain their types through native CALL. Each compound value is bounded to 64 KiB, eight levels and 128 children per container. The runtime validates host values before adding them to a frame.

CREATE TRIGGER declares BEFORE or AFTER INSERT, UPDATE or DELETE on a table FOR EACH ROW. The Server uses stable trigger object ID order within each timing and event. OLD and NEW are read-only row images. A missing image supplies NULL fields. Hosted INSERT, UPDATE and DELETE in procedures and triggers use the caller's transaction and current invoker permissions. The SQL parameter form `:NEW.column` or `:OLD.column` binds values from those images. A write-producing CALL in autocommit requires a request token. A trigger can neither commit independently nor return a cursor result.

The statement savepoint contains both the base row and nested trigger writes. An uncaught exception rolls back the complete statement and its WAL change set. Trigger recursion is bounded to eight levels. DROP TRIGGER removes a trigger descriptor, and a dependent trigger must be dropped before its table. The current subset runs row triggers for ordinary DML. TRUNCATE and DROP use their DDL history path.
