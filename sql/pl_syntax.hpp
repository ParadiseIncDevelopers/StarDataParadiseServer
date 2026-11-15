#pragma once

#include "syntax.hpp"

namespace sdp::pl
{
enum class Mode { in, out, in_out, local };

struct Variable
{
    sql::Name name;
    sql::Type type;
    Mode mode{};
    std::optional<sql::Expr> initial;
};

struct Node
{
    enum class Kind
    {
        assign, if_, while_, for_, loop_, exit_, continue_, return_, raise, block,
        select_into, open_cursor, fetch_cursor, close_cursor, emit_cursor, sql_mutation
    };

    Kind kind;
    sql::Source source;
    sql::Name name;
    std::optional<sql::Expr> first;
    std::optional<sql::Expr> second;
    std::vector<Node> body;
    std::vector<Node> alternate;
    std::vector<Node> handlers;
    std::string host_sql;
    std::vector<sql::Name> targets;
    std::string result_name;
};

struct Routine
{
    sql::Name name;
    bool function{};
    bool replace{};
    sql::Type returns;
    std::vector<Variable> variables;
    std::vector<Node> body;
    std::vector<Node> handlers;
    std::vector<std::string> cursors;
    std::vector<std::string> out_cursors;
    std::string source;
};
}
