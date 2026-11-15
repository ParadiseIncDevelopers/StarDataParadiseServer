# Typed PL/SQL IR v1

The compiler converts the parsed routine into structured instructions using indexed, typed frame slots. Function return and parameter modes are checked during compilation. Each instruction and expression stores a source location array with byte offset, 1-based line and 1-based column. The IR records its own format version and a decimal-string routine revision. These versions are independent of disk, WAL and wire formats.

A program holds the declared inputs, optional scalar return type, slot names and modes, typed body and exception branches, and SQL/determinism flags. Compiler-generated loop and declaration assignments use typed slots. SQL reads retain original SELECT source and a mapping of lexical name to slot and type. Execution binds values into that mapping. The same mapping can support debugger locals and a future source exporter.

The catalog stores canonical routine source, serialized IR and stable dependency object IDs with their revision numbers. During invocation the Server checks the dependency versions, recompiles the stored source and checks the serialized IR against the durable record before execution. Each invocation uses one immutable compiled revision. Replacing a routine writes a new version of that catalog record through the same WAL transaction as other SQL DDL. The stage limits routine source to 8 KiB and its combined persisted definition to 16 KiB.

The runtime reports typed exceptions with the instruction's source location. User RAISE and SQL value errors can enter an EXCEPTION handler. Cancellation and resource-limit failures propagate to the caller.

Etap 17 adds host instructions for SQL statements, cursor names, result names and typed target slot ordinals. These fields are populated for the corresponding opcodes. Programs can declare local and output cursors and report whether they contain SQL writes. IR v1 serialization of an Etap 16 program without these features retains its original shape.
