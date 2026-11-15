#pragma once

#include "pl_syntax.hpp"

#include <functional>
#include <span>

namespace sdp::pl
{
inline constexpr unsigned ir_version = 1;

struct Binding
{
    std::string name;
    sql::Type type;
    size_t slot;
};

struct TypedExpr
{
    sql::Expr::Op op{};
    sql::Source source;
    sql::Type type;
    disk::Value literal;
    size_t slot{};
    std::string text;
    sql::Name name;
    std::vector<TypedExpr> args;
    std::vector<Binding> bindings;
};

struct Slot
{
    std::string name;
    sql::Type type;
    Mode mode{};
    sql::Source source;
};

struct Instruction
{
    Node::Kind kind{};
    sql::Source source;
    size_t slot{};
    std::optional<TypedExpr> first, second;
    std::vector<Instruction> body, alternate, handlers;
    std::string exception;
    std::string host_sql, cursor, result_name;
    std::vector<size_t> targets;
    std::vector<Binding> bindings;
};

struct Program
{
    sql::Name name;
    uint64_t revision{1};
    std::optional<sql::Type> returns;
    size_t parameters{};
    std::vector<Slot> slots;
    std::vector<Instruction> body, handlers;
    bool uses_sql{};
    bool deterministic{true};
    bool writes_sql{};
    std::vector<std::string> cursors, out_cursors;
};

// Bind SQL expressions in the same catalog and rights context as the caller.
using HostType = std::function<sql::Type(const sql::Expr&, std::span<const sql::Type>, std::span<const Binding>)>;
Program compile(const Routine& routine, uint64_t revision = 1, const HostType& host = {});
Program compile_expression(std::span<const Variable> inputs, const sql::Expr& expression, const HostType& host = {});
std::string serialize(const Program& program);
}
