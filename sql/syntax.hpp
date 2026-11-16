#pragma once

#include "diagnostic.hpp"
#include "types.hpp"

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace sdp::pl { struct Routine; }

namespace sdp::sql
{
struct Name
{
    std::vector<std::string> parts;
    Source source;
};

struct Statement;

struct Expr
{
    enum class Op
    {
        null, boolean, integer, decimal, text, date, time, timestamp_tz, name, parameter, negate,
        logical_not, and_, or_, eq, ne, lt, le, gt, ge, is_null, is_not_null, next_value,
        aggregate, exists, subquery, add, subtract, multiply, divide, routine_call
    };

    Op op;
    Source source;
    std::string text;
    Name name;
    std::vector<Expr> args;
    std::shared_ptr<Statement> query;
};

struct ColumnDefinition
{
    Name name;
    Type type;
    bool nullable = true;
    std::optional<Expr> default_value;
    bool identity{};
};

enum class Action
{
    create_database, drop_database, create_schema, drop_schema, create_table, drop_table, add_column, alter_table,
    select, insert, update, delete_, create_index, drop_index, rename_index, truncate_table,
    create_view, drop_view, create_sequence, drop_sequence, explain, explain_analyze, analyze, reindex,
    create_user, create_role, grant, revoke, create_routine, call_routine, pl_block,
    create_trigger, drop_trigger, history, create_decision_rule, activate_decision_rule, decision_inspect,
    model_command
};

struct Assignment
{
    Name column;
    Expr value;
};

struct DecisionRule
{
    enum class Event { insert, update, delete_, write };
    enum class Effect { allow, deny, compute };

    Event event{};
    Effect effect{};
    uint32_t priority{};
    uint32_t version{};
    std::optional<Expr> condition;
    std::optional<Assignment> assignment;
    std::string definition;
};

struct DecisionInspection
{
    enum class Kind { explain, whynot, whatif, test, recheck, validate };

    Kind kind;
    std::string body;
};

struct ModelCommand
{
    enum class Kind { create, train, evaluate, predict, activate };
    Kind kind;
    std::string body;
};

struct ConstraintDefinition
{
    enum class Kind { primary_key, unique, foreign_key, check };

    Kind kind;
    std::vector<Name> columns;
    Name referenced_table;
    std::vector<Name> referenced_columns;
    std::optional<Expr> predicate;
    std::optional<std::string> name;
    Source source;
};

struct AlterTable
{
    enum class Kind { add_column, drop_column, rename_column, change_type, set_nullability,
        set_default, drop_default, add_identity, drop_identity, add_constraint, drop_constraint };
    Kind kind{};
    Name subject;
    Name replacement;
    Type type;
    bool nullable{};
    std::optional<Expr> default_value;
    std::optional<size_t> constraint_ordinal;
};

struct JoinClause
{
    enum class Kind { inner, left, cross };

    Kind kind;
    Name target;
    std::optional<std::string> alias;
    std::optional<Expr> on;
};

struct OrderTerm
{
    Expr expression;
    bool descending{};
    std::string collation;
};

struct UnionArm
{
    bool all{};
    std::shared_ptr<Statement> query;
};

struct Cte
{
    std::string name;
    std::shared_ptr<Statement> query;
};

struct HistoryCommand
{
    enum class Kind { log, diff, read, verify, status, retain, purge, pin, unpin, restore, revert };

    Kind kind;
    Name target;
    uint64_t revision{}, from{}, to{}, row_id{}, quota_bytes{};
    uint32_t offset{}, limit{};
    std::optional<int64_t> since_utc, until_utc;
    std::string pin_kind, pin_name;
    std::string field, plan_id, request_token;
};

struct Statement
{
    Action action;
    Source source;
    Name target;
    std::vector<ColumnDefinition> columns;
    std::vector<Expr> projection;
    std::optional<Expr> filter;
    std::optional<std::string> alias;
    bool all_columns{};
    std::vector<Name> insert_columns;
    std::vector<std::vector<Expr>> values;
    std::vector<Assignment> assignments;
    Name related;
    std::vector<Name> key_columns;
    bool unique_index{};
    std::string view_sql;
    std::vector<ConstraintDefinition> constraints;
    std::optional<AlterTable> alteration;
    std::vector<JoinClause> joins;
    std::vector<Expr> group_by;
    std::optional<Expr> having;
    std::vector<OrderTerm> order_by;
    bool distinct{};
    std::vector<UnionArm> unions;
    std::vector<Cte> ctes;
    std::shared_ptr<Statement> query;
    std::string collation;
    std::vector<std::string> permissions;
    std::string security_kind;
    std::string security_subject;
    bool security_role{};
    std::shared_ptr<pl::Routine> routine;
    std::string trigger_timing;
    std::string trigger_event;
    std::optional<HistoryCommand> history;
    std::optional<DecisionRule> decision;
    std::optional<DecisionInspection> decision_inspection;
    std::optional<ModelCommand> model;
};

Statement parse(std::string_view sql);
Name parse_history_target(std::string_view target);
}
