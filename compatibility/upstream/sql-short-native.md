# Native short SQL/PLSQL implementation

The stable 0.5.0 SQL/PLSQL feature set runs in the native compiler and runtime
of the pinned MySQL 26.10.0 source. The six execution gates below passed against
the immutable build13 engine; FULL joins with typed Knowledge inputs also
passed the complete build12c context gate. Bison/MSVC syntax checks are separate
checks and do not substitute for these execution results. There is no new SQL
dialect, `COMPACT` command or session mode.

The final release source package uses the pinned upstream revision and the
single primary `server-26.10.0.patch`. Intermediate SQL feature diffs are
development provenance, not additional installation steps; never apply them on
top of the combined primary patch. The ignored development tree is
`out/mysql-migration/server-upstream-baseline`. Snapshot export verifies clean
application and source-hash reproduction. HIST/git and new public SQL JOB syntax
are deferred and excluded from stable 0.5.0. Existing legacy compute jobs and
the private AI work queue retain their established scope.

Product errors carry public message prefixes `630001`–`630006`; their Classic
protocol error numbers remain `63001`–`63006`. Existing native errors such as
1054/1052/1060 retain their established ABI and messages.

## Stable 0.5.0 execution gates

All six fixtures passed with AI/actions OFF, fresh TLS datadirs and clean
shutdown against immutable build13
`9a16886bd3b846f3c45f6af39cbd2de9455b64e16706c68b2b899d38cccf64c4`.

| Native fixture under `compatibility.tests` | Accepted coverage |
| --- | --- |
| `sql_short_native_acceptance` | Projection expansion/ACL, native procedural forms, typed locals/cursors, errors and cleanup |
| `sql_short_relational_acceptance` | InnoDB QUALIFY plans, named insertion and pipeline boundaries |
| `sql_short_inspection_acceptance` | SHOW EXPANDED metadata/IR/source maps, prepared statements, reprepare and restart persistence |
| `sql_short_named_insert_acceptance` | Updatable views, ODKU, prepared/UNION sources, triggers, rollback and ACL |
| `sql_short_order_acceptance` | ORDER continuation boundaries, volatile key evaluation, prepared statements, views and name collisions |
| `full_outer_native_acceptance` | Bag/NULL semantics, volatile expressions, limits, prepared/view reopen/empty/reset, payload metadata and ACL |

The six-fixture result and individual logs are recorded in
`out/sql-short-native/build13-regression-summary.json`. Trusted Knowledge inputs
on both FULL sides and repeated typed prepared rebinds passed
`ai_context_native --full` against immutable12c; see the validation record below.
The supported operand and optimizer limits documented here remain in force.

## Integrated source milestones

The build6/build7 statuses in this table record the initial integration
milestones. The stable execution acceptance is the six-gate record above.

| Form | Native compiler/runtime path | Execution status |
| --- | --- | --- |
| `SELECT [t.]* EXCLUDE (...) REPLACE (... AS name)` | `Item_asterisk`, ordinary `insert_fields` visibility/ACL expansion, post-expansion projection binding | PASS — build6 |
| `LET name [type] = expression` | Native SP declaration, `Create_field`, expression preparation and `sp_instr_set` | PASS — build6 |
| `WHEN expression DO statement` | Native conditional jump and continuation/backpatch instructions | PASS — build6 |
| `AS RETURN expression` | Existing native function return instruction/type conversion | PASS — build6 |
| `AS ... END` routine body | Balanced native statement-list scope, handlers and cursor cleanup | PASS — build6 |
| `:local`, `:row.field` | Lexer prefix token and native `Item_splocal`, including source positions for binary logging | PASS — build6 |
| `FOR row IN query DO ... END FOR` | Native server-side cursor, one cached typed row, conditional fetch and scoped cursor cleanup | PASS — build6 |
| `SHOW EXPANDED query`, `SHOW EXPANDED PROCEDURE/FUNCTION name` | Prepared/bound native query printer, output metadata, or real SP instructions labelled `compiler_ir` | PASS — build6 |
| `QUALIFY expression` | Legacy optimizer filter after the final window, before DISTINCT, sorting and LIMIT | PASS — build7 (documented initial scope) |
| `INSERT INTO target BY NAME query` | Source expansion/type-aware target binding and native INSERT execution | PASS — build7 (documented initial scope) |
| `FROM ... |> stage` | Native relational AST lowering and persisted materialization boundaries | PASS — build7 (documented initial scope) |

`EXCLUDE` and `REPLACE` match output names using the engine's identifier
comparison. Missing, repeated and ambiguous targets are errors. Replacements
retain the output name and position; their expressions bind the original input
namespace. An excluded column cannot be replaced. Removing every column is an
error. Existing star column permissions remain in force, including excluded
columns; exclusion does not grant access to a restricted star.

LET initializers run when execution reaches the declaration. Inferred storage
uses expression **metadata during native lazy statement preparation**, before
the initializer's value is evaluated. NULL-only initialization requires an
explicit type. The slot's type remains fixed during the call; later assignments
use ordinary native conversion/overflow checks. DECIMAL scale and inferred string
length are retained. Inference currently requires an explicit declaration for
ENUM, SET, GEOMETRY and VECTOR expressions. A fresh routine call uses current
schema metadata through the native reprepare path.

LET declarations become visible after their initializer is itemized. Duplicate
declarations in the same scope fail; an inner scope can shadow an outer name.
Short routine bodies and short locals require explicit `:name` references in
embedded SQL. Procedural expressions and SET right-hand sides can use bare local
names. Existing long-body local precedence remains available for existing
DECLARE variables.

FOR query output names must be unique. Field metadata comes from native cursor
preparation; field references bind to the typed cursor row. The row is read only
and scoped to the loop. Empty cursors skip the body. Native cursor storage may
materialize/spill as it already does for DECLARE CURSOR; this implementation adds
only one row of cached values. The loop name is also a native label for
`LEAVE name` and `ITERATE name`. RETURN and exceptional scope exit use existing
cursor stack cleanup. `DO` is a delimiter only while lexing this new FOR query;
use a quoted identifier for a column named DO within that query. Normal SQL DO
identifiers and legacy labels retain their lexer tokens.

SHOW EXPANDED prints parameters as placeholders and prepares SELECT queries
without optimizing/executing a plan. Its SELECT grammar excludes top-level INTO
and locking tails. Routine output is compiler IR, not executable SQL. Routine
inspection uses native source-inspection privileges. The integrated source
supplies binary prepared-result metadata, AST byte source maps and the durable
native DD language-version property. Its actual TLS binary-protocol and
server-restart fixture passed again on build12; text protocol results
alone were not used as evidence for those contracts.

## Relational source integration

QUALIFY, INSERT BY NAME and FROM pipelines are integrated in the canonical native
source and consolidated into the primary source patch. Their acceptance fixture is
`compatibility.tests.sql_short_relational_acceptance`.

QUALIFY adds a filter immediately after the final legacy window
iterator, before DISTINCT materialization, final sorting and LIMIT. It also
disables early row-limit pushdown. Removing the hypergraph-only parser gate by
itself is not the implementation. Source columns take precedence over a
same-named SELECT alias, using the existing resolver rule.

BY NAME binds targets after source-star expansion and before source
expression type resolution, so native target typing also applies to parameters
and UNION operands. It uses native INSERT execution and defaults, generated
columns, constraints and triggers. Its initial source contract accepts base
tables and query insertions. Accepted support now also covers single-base
updatable views, including aliases/nested views and CHECK OPTION, and explicit
ON DUPLICATE KEY UPDATE. Native triggers/rollback/column ACL remain in force;
there is no automatic upsert.

Pipelines create native query/derived-table ASTs, materialize stage
boundaries and disable cross-boundary condition pushdown. Each derived stage has
an explicit maximal LIMIT barrier unless the stage has its own limit, preserving
the boundary when canonical view SQL is reparsed. Initial stages are
FROM, WHERE, SELECT with DISTINCT/ALL, EXTEND, AGGREGATE with GROUP BY, explicit
JOIN with ON/USING, ORDER BY and LIMIT. Incoming columns are available under the
stage alias `input`; previous source table aliases end at a stage boundary.
Group keys and aggregate expressions form the AGGREGATE output schema. EXTEND
siblings do not see each other's aliases; volatile expressions are materialized
once per input row. ORDER continuation materializes scalar sort keys once, then
carries a native ROW_NUMBER ordinal through preserving stages. Compiler names
are collision-safe and hidden by source identity; user positional sorting never
includes compiler columns. DISTINCT/AGGREGATE/JOIN discard the old order/carrier.
LIMIT followed by ORDER remains a separate materialized boundary. Single and
multiple volatile sort keys, prepared statements/views and collisions passed
the complete build12 suite.

## Validation record

20 November 2026: integrated native Bison checks pass with the upstream
`%expect 37`, without new unresolved conflicts. Standalone MSVC `/Zs` checks pass
for `item.cc`, `sql_resolver.cc`, `parse_tree_items.cc`, `sp_rcontext.cc` and the
short-SP/FOR/expanded helper set. QUALIFY and BY NAME overlay source syntax checks
also pass. Pipeline helper Bison and `/Zs` checks pass. Logs are under
`out/sql-short-native`; these are syntax checks only.

The combined source snapshot uses LF patch line endings and verifies clean
application/source reproduction while retaining coordinated AI/Knowledge and
VERSION() changes. Intermediate feature patches are superseded provenance;
HIST is excluded from the current release boundary.

Run against the newly built engine, without client SQL rewriting:

```powershell
python -B -m compatibility.tests.sql_short_native_acceptance
python -B -m compatibility.tests.sql_short_relational_acceptance
```

The first fixture covers projection equivalence, errors, ACL/visibility,
CTE/set/view/prepared statements/DDL, procedural types and scopes, scalar-query
cardinality, control flow, row cursors and compiler inspection. The second covers
real InnoDB window plans and relational equivalence, named insertion and pipeline
boundaries, volatility, views and prepared parameters. Actual execution results
are recorded below.

Actual acceptance: the full native fixture and the new
`compatibility.tests.sql_short_inspection_acceptance` passed with clean shutdown
against immutable build6 and fresh datadirs. The relational fixture passed in
full against immutable build7 after fixing a pipeline GROUP BY output-name bug:
an absent alias must use `{nullptr,0}`, whereas `EMPTY_CSTR` requests an explicit
empty name. Logs are `out/sql-short-native/build6-*.log` and
`out/sql-short-native/build7-sql_short_relational_acceptance.log`.

Historical build12 acceptance: immutable build12
`4ec267cb8444f8ecfe5f4c314d4422af69f1769159198e234a10c1216634b8e9`
passed the complete native, relational, inspection/restart, named-insert/view/
ODKU, ORDER continuation and production AI-local-capture suites. All used fresh
TLS datadirs and clean shutdown. The integrated grammar now uses `%expect36`
without new unresolved conflicts. Earlier build6/7 results above are historical.

Explicit native `FULL OUTER JOIN` uses a separate AST/plan/disk-spool RowIterator,
never a textual UNION rewrite. Its supported slice accepts two base
or trusted typed Knowledge inputs, scalar ON and post-join WHERE/sort/LIMIT.
Arbitrary derived/CTE/view operands, multijoin/lateral shapes, grouping/windows/
DISTINCT/subqueries and locking reads fail explicitly. Bare FULL alias plus
JOIN retains existing INNER semantics. Build12 passed base bag/NULL/16 graph/
volatile ON/filter/sort/LIMIT/PS checks, then crashed on materialized view reset
because the legacy cleanup assumed QEP_TAB. A FULL-only cleanup fix followed
in12a; full acceptance was still pending at that milestone. The subsequent
payload/plan/ACL/empty diagnostic checks omitted the known view/SP reset gates
and did not constitute FULL feature acceptance. The complete12c and13 gates
below/above supersede that diagnostic result. The final fixture covers repeated
view reopen and prepared view
empty-intermediate/reset/deallocate paths.

Detailed successes, failures and preconditions are recorded in
`docs/coordination/SQL_SHORT_NATIVE_2026_11_20.md`. The final primary release patch
is regenerated after acceptance; no intermediate diff is an independent source
package.

Latest FULL execution acceptance: immutable12c
`a5d465ddc8580090418f870d28eb05c8e2d9257e21907a589a94a1820c463d66`
passed the complete extended FULL fixture and `ai_context_native --full`, both
with fresh TLS data and clean shutdown. This includes trusted Knowledge inputs
on each side, repeated PS2→uint64MAX→2→empty→2, exact uint64 metadata and unmatched
rows. Initializing both source iterators before the first base-table row avoids
starting an autocommit nonlocking transaction before Knowledge's locking catalog
read. Native locks/cleanup remain intact; the final fixtures do not wrap the
query in an explicit-transaction workaround. The previously documented12/12a
failures are retained as provenance, not current unresolved FULL gates.
