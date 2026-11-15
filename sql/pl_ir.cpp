#include "pl_ir.hpp"
#include "wire_values.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace sdp::pl
{
namespace
{
using sql::Code;
using sql::Error;
using sql::Kind;
using sql::Name;
using sql::Source;
using sql::Type;
using SqlExpr = sql::Expr;
using Op = SqlExpr::Op;
using Statement = Node::Kind;
using wire::Json;

constexpr Type boolean_type{Kind::boolean};
constexpr Type integer_type{Kind::integer};
constexpr size_t max_slots = 128;
constexpr size_t max_nodes = 2048;
constexpr size_t max_depth = 48;
constexpr size_t max_host_bindings = 4096;
constexpr size_t max_ir_bytes = 256 * 1024;

std::string identifier(const Name& name)
{
    if (name.parts.size() != 1 || name.parts.front().empty())
        throw Error(Code::name, name.source, "Expected an unqualified PL/SQL identifier");
    return name.parts.front();
}

std::string variable_identifier(const Name& name)
{
    if (name.parts.size() == 2 && !name.parts[1].empty() &&
        (name.parts[0] == "old" || name.parts[0] == "new"))
        return name.parts[0] + "." + name.parts[1];
    return identifier(name);
}

void require_conversion(Type type, Type target, Source source)
{
    if (!sql::can_coerce(type, target))
        throw Error(Code::type, source, "Cannot assign " + sql::type_name(type) + " to " + sql::type_name(target));
}

void require_concrete(Type type, Source source)
{
    sql::validate_type(type, source);
    if (type.kind == Kind::null) throw Error(Code::type, source, "A declared PL/SQL type cannot be NULL");
}

struct Scope
{
    size_t loops{};
    bool handler{};
};

struct Symbol
{
    size_t slot;
    bool writable;
};

class Compiler
{
public:
    Compiler(const Routine& routine, uint64_t revision, const HostType& host)
        : routine_(routine), host_(host)
    {
        program_.name = routine_.name;
        program_.revision = revision;
    }

    Program run(const SqlExpr* scalar = nullptr)
    {
        if (!program_.revision) throw Error(Code::type, routine_.name.source, "Invalid zero routine revision");
        if (routine_.function)
        {
            require_concrete(routine_.returns, routine_.name.source);
            program_.returns = routine_.returns;
        }

        bool declared_locals{};
        for (const auto& variable : routine_.variables)
        {
            const auto name = variable_identifier(variable.name);
            require_concrete(variable.type, variable.name.source);
            if (symbols_.contains(name))
                throw Error(Code::name, variable.name.source, "Duplicate PL/SQL declaration");

            if (variable.mode == Mode::local) declared_locals = true;
            else
            {
                if (declared_locals || variable.initial)
                    throw Error(Code::type, variable.name.source, "Parameter declarations must precede locals");
                ++program_.parameters;
            }

            std::optional<TypedExpr> initial;
            if (variable.initial)
            {
                initial = expression(*variable.initial, 0);
                require_conversion(initial->type, variable.type, initial->source);
            }
            const auto slot = allocate(name, variable.type, variable.mode, variable.name.source);
            symbols_.emplace(name, Symbol{slot, variable.mode != Mode::in && variable.name.parts.size() == 1});
            if (initial)
            {
                count(variable.name.source, 0);
                Instruction assignment{};
                assignment.kind = Statement::assign;
                assignment.source = variable.name.source;
                assignment.slot = slot;
                assignment.first = std::move(initial);
                program_.body.push_back(std::move(assignment));
            }
        }

        declare_cursors(routine_.cursors, program_.cursors);
        declare_cursors(routine_.out_cursors, program_.out_cursors);
        auto body = block(routine_.body, {}, 0);
        for (auto& instruction : body) program_.body.push_back(std::move(instruction));
        program_.handlers = block(routine_.handlers, {0, true}, 0);
        if (scalar)
        {
            Instruction result{};
            result.kind = Statement::return_;
            result.source = scalar->source;
            result.first = expression(*scalar, 0);
            program_.returns = result.first->type;
            program_.body.push_back(std::move(result));
        }
        return std::move(program_);
    }

private:
    void count(Source source, size_t depth)
    {
        if (++nodes_ > max_nodes || depth >= max_depth)
            throw Error(Code::limit, source, "PL/SQL IR size or nesting limit exceeded");
    }

    size_t allocate(std::string name, Type type, Mode mode, Source source)
    {
        if (program_.slots.size() >= max_slots) throw Error(Code::limit, source, "PL/SQL slot limit exceeded");
        const auto index = program_.slots.size();
        program_.slots.push_back({std::move(name), type, mode, source});
        return index;
    }

    Symbol resolve(const Name& name) const
    {
        const auto result = symbols_.find(variable_identifier(name));
        if (result == symbols_.end()) throw Error(Code::name, name.source, "Unknown PL/SQL variable");
        return result->second;
    }

    std::vector<Binding> capture_bindings(Source source)
    {
        if (symbols_.size() > max_host_bindings - bindings_)
            throw Error(Code::limit, source, "PL/SQL host binding budget exceeded");
        bindings_ += symbols_.size();
        std::vector<Binding> result;
        result.reserve(symbols_.size());
        for (const auto& [name, symbol] : symbols_)
            result.push_back({name, program_.slots[symbol.slot].type, symbol.slot});
        return result;
    }

    TypedExpr host_expression(TypedExpr out, const SqlExpr& raw)
    {
        if (!host_) throw Error(Code::unsupported, raw.source, "This PL/SQL SQL or routine expression needs a catalog");
        if (out.op != Op::routine_call && (out.text.empty() || !raw.query || raw.query->action != sql::Action::select))
            throw Error(Code::unsupported, raw.source, "PL/SQL query expressions require a SELECT");
        std::vector<Type> types;
        types.reserve(out.args.size());
        for (const auto& arg : out.args) types.push_back(arg.type);
        out.bindings = capture_bindings(raw.source);
        out.type = host_(raw, types, out.bindings);
        sql::validate_type(out.type, raw.source);
        if (out.op == Op::exists && out.type != boolean_type)
            throw Error(Code::type, raw.source, "EXISTS requires a BOOLEAN host type");
        program_.uses_sql = true;
        program_.deterministic = false;
        return out;
    }

    TypedExpr expression(const SqlExpr& raw, size_t depth)
    {
        count(raw.source, depth);
        TypedExpr out{};
        out.op = raw.op;
        out.source = raw.source;
        out.args.reserve(raw.args.size());
        for (const auto& arg : raw.args) out.args.push_back(expression(arg, depth + 1));
        const auto arity = [&](size_t expected) {
            if (out.args.size() != expected) throw Error(Code::type, raw.source, "Invalid PL/SQL operand count");
        };

        switch (raw.op)
        {
        case Op::name:
        case Op::parameter:
        {
            arity(0);
            const auto name = raw.op == Op::name ? raw.name : Name{{raw.text}, raw.source};
            out.op = Op::name;
            out.slot = resolve(name).slot;
            out.type = program_.slots[out.slot].type;
            return out;
        }
        case Op::null: out.type = {Kind::null}; break;
        case Op::boolean: out.type = boolean_type; break;
        case Op::integer: out.type = integer_type; break;
        case Op::decimal: out.type = sql::decimal_literal_type(raw.text, raw.source); break;
        case Op::text: out.type = {Kind::text}; break;
        case Op::date: out.type = {Kind::date}; break;
        case Op::time: out.type = {Kind::time}; break;
        case Op::timestamp_tz: out.type = {Kind::timestamp_tz}; break;
        case Op::logical_not:
        case Op::and_:
        case Op::or_:
            arity(raw.op == Op::logical_not ? 1 : 2);
            for (const auto& arg : out.args) require_conversion(arg.type, boolean_type, arg.source);
            out.type = boolean_type;
            return out;
        case Op::eq:
        case Op::ne:
        case Op::lt:
        case Op::le:
        case Op::gt:
        case Op::ge:
            arity(2);
            if (!sql::comparable(out.args[0].type, out.args[1].type))
                throw Error(Code::type, raw.source, "Comparison operands have incompatible types");
            out.type = boolean_type;
            return out;
        case Op::is_null:
        case Op::is_not_null:
            arity(1);
            out.type = boolean_type;
            return out;
        case Op::negate:
            arity(1);
            out.type = out.args.front().type;
            if (out.type.kind != Kind::null && out.type.kind != Kind::integer && out.type.kind != Kind::decimal)
                throw Error(Code::type, raw.source, "Negation requires a numeric value");
            return out;
        case Op::add:
        case Op::subtract:
        case Op::multiply:
        case Op::divide:
            arity(2);
            for (const auto& arg : out.args) require_conversion(arg.type, integer_type, arg.source);
            out.type = integer_type;
            return out;
        case Op::exists:
        case Op::subquery:
            arity(0);
            out.text = raw.text;
            return host_expression(std::move(out), raw);
        case Op::routine_call:
            out.name = raw.name;
            return host_expression(std::move(out), raw);
        default:
            throw Error(Code::unsupported, raw.source, "Expression is unavailable in PL/SQL");
        }

        arity(0);
        out.literal = sql::parse_literal(out.type, raw.text, raw.source);
        return out;
    }

    std::vector<Instruction> block(const std::vector<Node>& tree, Scope scope, size_t depth)
    {
        std::vector<Instruction> result;
        for (const auto& node : tree) result.push_back(instruction(node, scope, depth));
        return result;
    }

    static void required(const std::optional<TypedExpr>& expression, Source source)
    {
        if (!expression) throw Error(Code::syntax, source, "PL/SQL expression is required");
    }

    void condition(const std::optional<TypedExpr>& expression, Source source)
    {
        required(expression, source);
        require_conversion(expression->type, boolean_type, expression->source);
    }

    void declare_cursors(const std::vector<std::string>& names, std::vector<std::string>& storage)
    {
        for (const auto& name : names)
        {
            if (cursor_names_.size() == 32)
                throw Error(Code::limit, routine_.name.source, "Routine cursor limit exceeded");
            if (name.empty() || symbols_.contains(name) || !cursor_names_.insert(name).second)
                throw Error(Code::name, routine_.name.source, "Duplicate PL/SQL cursor declaration");
            storage.push_back(name);
        }
    }

    void host_instruction(Instruction& out, const Node& node)
    {
        const bool takes_sql = node.kind == Statement::open_cursor || node.kind == Statement::select_into ||
            node.kind == Statement::sql_mutation;
        const bool row_targets = node.kind == Statement::select_into || node.kind == Statement::fetch_cursor;
        if (node.kind != Statement::select_into && node.kind != Statement::sql_mutation)
        {
            out.cursor = identifier(node.name);
            if (!cursor_names_.contains(out.cursor))
                throw Error(Code::name, node.name.source, "Unknown PL/SQL cursor");
        }
        if (takes_sql)
        {
            if (node.host_sql.empty()) throw Error(Code::syntax, node.source, "SQL source is required");
            const auto parsed = sql::parse(node.host_sql);
            if (node.kind == Statement::sql_mutation)
            {
                if (routine_.function || (parsed.action != sql::Action::insert && parsed.action != sql::Action::update &&
                    parsed.action != sql::Action::delete_))
                    throw Error(Code::unsupported, node.source, "Only procedure and trigger CRUD statements are allowed");
                program_.writes_sql = true;
            }
            else if (parsed.action != sql::Action::select)
                throw Error(Code::unsupported, node.source, "Cursor queries require SELECT");
            out.host_sql = node.host_sql;
            out.bindings = capture_bindings(node.source);
        }
        if (row_targets)
        {
            if (node.targets.empty()) throw Error(Code::syntax, node.source, "INTO requires output variables");
            for (const auto& target : node.targets)
            {
                const auto symbol = resolve(target);
                if (!symbol.writable) throw Error(Code::type, target.source, "INTO variable is read-only");
                if (std::ranges::find(out.targets, symbol.slot) != out.targets.end())
                    throw Error(Code::name, target.source, "Duplicate INTO variable");
                out.targets.push_back(symbol.slot);
            }
        }
        if (node.kind == Statement::emit_cursor)
            out.result_name = node.result_name.empty() ? out.cursor : node.result_name;
        program_.uses_sql = true;
        program_.deterministic = false;
    }

    std::vector<Instruction> for_body(const Node& node, size_t slot, Scope scope, size_t depth)
    {
        const auto name = identifier(node.name);
        const auto existing = symbols_.find(name);
        const std::optional<Symbol> outer = existing == symbols_.end() ? std::nullopt : std::optional(existing->second);
        symbols_.insert_or_assign(name, Symbol{slot, false});
        auto result = block(node.body, {scope.loops + 1, scope.handler}, depth + 1);
        if (outer) symbols_.insert_or_assign(name, *outer);
        else symbols_.erase(name);
        return result;
    }

    Instruction instruction(const Node& node, Scope scope, size_t depth)
    {
        count(node.source, depth);
        Instruction out{};
        out.kind = node.kind;
        out.source = node.source;
        if (node.first) out.first = expression(*node.first, depth + 1);
        if (node.second) out.second = expression(*node.second, depth + 1);
        switch (node.kind)
        {
        case Statement::assign:
        {
            required(out.first, node.source);
            const auto symbol = resolve(node.name);
            if (!symbol.writable) throw Error(Code::type, node.name.source, "PL/SQL variable is read-only");
            require_conversion(out.first->type, program_.slots[symbol.slot].type, out.first->source);
            out.slot = symbol.slot;
            break;
        }
        case Statement::if_:
            condition(out.first, node.source);
            out.body = block(node.body, scope, depth + 1);
            out.alternate = block(node.alternate, scope, depth + 1);
            break;
        case Statement::while_:
            condition(out.first, node.source);
            [[fallthrough]];
        case Statement::loop_:
            out.body = block(node.body, {scope.loops + 1, scope.handler}, depth + 1);
            break;
        case Statement::for_:
            required(out.first, node.source);
            required(out.second, node.source);
            require_conversion(out.first->type, integer_type, out.first->source);
            require_conversion(out.second->type, integer_type, out.second->source);
            out.slot = allocate(identifier(node.name), integer_type, Mode::local, node.name.source);
            out.body = for_body(node, out.slot, scope, depth);
            break;
        case Statement::exit_:
        case Statement::continue_:
            if (!scope.loops) throw Error(Code::syntax, node.source, "Loop control requires a loop");
            if (out.first) condition(out.first, node.source);
            break;
        case Statement::return_:
            if (program_.returns)
            {
                required(out.first, node.source);
                require_conversion(out.first->type, *program_.returns, out.first->source);
            }
            else if (out.first) throw Error(Code::type, node.source, "A procedure cannot return a value");
            break;
        case Statement::raise:
            if (!node.name.parts.empty()) out.exception = identifier(node.name);
            else if (!scope.handler) throw Error(Code::syntax, node.source, "RAISE alone requires an exception handler");
            break;
        case Statement::block:
            out.body = block(node.body, scope, depth + 1);
            out.handlers = block(node.handlers, {scope.loops, true}, depth + 1);
            break;
        case Statement::select_into:
        case Statement::open_cursor:
        case Statement::fetch_cursor:
        case Statement::close_cursor:
        case Statement::emit_cursor:
        case Statement::sql_mutation:
            host_instruction(out, node);
            break;
        }
        return out;
    }

    const Routine& routine_;
    const HostType& host_;
    Program program_{};
    std::map<std::string, Symbol, std::less<>> symbols_;
    std::set<std::string, std::less<>> cursor_names_;
    size_t nodes_{};
    size_t bindings_{};
};

Json write_source(Source source)
{
    return Json::array({source.offset, source.line, source.column});
}

Json write_type(Type type)
{
    Json result{{"name", sql::type_name(type)}};
    if (type.kind == Kind::decimal)
    {
        result["precision"] = type.precision;
        result["scale"] = type.scale;
    }
    return result;
}

std::string_view mode_name(Mode mode)
{
    switch (mode)
    {
    case Mode::in: return "in";
    case Mode::out: return "out";
    case Mode::in_out: return "in_out";
    case Mode::local: return "local";
    }
    throw Error(Code::unsupported, {}, "Unknown PL/SQL slot mode");
}

std::string_view instruction_name(Statement kind)
{
    switch (kind)
    {
    case Statement::assign: return "assign";
    case Statement::if_: return "if";
    case Statement::while_: return "while";
    case Statement::for_: return "for";
    case Statement::loop_: return "loop";
    case Statement::exit_: return "exit";
    case Statement::continue_: return "continue";
    case Statement::return_: return "return";
    case Statement::raise: return "raise";
    case Statement::block: return "block";
    case Statement::select_into: return "select_into";
    case Statement::open_cursor: return "open_cursor";
    case Statement::fetch_cursor: return "fetch_cursor";
    case Statement::close_cursor: return "close_cursor";
    case Statement::emit_cursor: return "emit_cursor";
    case Statement::sql_mutation: return "sql_mutation";
    }
    throw Error(Code::unsupported, {}, "Unknown PL/SQL instruction");
}

std::string_view expression_name(Op op)
{
    switch (op)
    {
    case Op::null: return "literal";
    case Op::boolean: return "literal";
    case Op::integer: return "literal";
    case Op::decimal: return "literal";
    case Op::text: return "literal";
    case Op::date: return "literal";
    case Op::time: return "literal";
    case Op::timestamp_tz: return "literal";
    case Op::name: return "load";
    case Op::logical_not: return "not";
    case Op::and_: return "and";
    case Op::or_: return "or";
    case Op::eq: return "eq";
    case Op::ne: return "ne";
    case Op::lt: return "lt";
    case Op::le: return "le";
    case Op::gt: return "gt";
    case Op::ge: return "ge";
    case Op::is_null: return "is_null";
    case Op::is_not_null: return "is_not_null";
    case Op::negate: return "negate";
    case Op::add: return "add";
    case Op::subtract: return "subtract";
    case Op::multiply: return "multiply";
    case Op::divide: return "divide";
    case Op::exists: return "exists";
    case Op::subquery: return "subquery";
    case Op::routine_call: return "call";
    default: throw Error(Code::unsupported, {}, "Unknown PL/SQL operand");
    }
}

Json write_expression(const TypedExpr& expression)
{
    const auto operation = expression_name(expression.op);
    Json result{{"op", operation}, {"type", write_type(expression.type)}, {"at", write_source(expression.source)}};
    if (operation == "literal") result["value"] = sql::write_value(expression.literal, expression.type);
    if (expression.op == Op::name) result["slot"] = expression.slot;
    if (!expression.text.empty()) result["sql"] = expression.text;
    if (!expression.name.parts.empty()) result["routine"] = expression.name.parts;
    if (!expression.args.empty())
    {
        auto args = Json::array();
        for (const auto& arg : expression.args) args.push_back(write_expression(arg));
        result["args"] = std::move(args);
    }
    if (!expression.bindings.empty())
    {
        auto bindings = Json::array();
        for (const auto& slot : expression.bindings)
            bindings.push_back({{"name", slot.name}, {"slot", slot.slot}, {"type", write_type(slot.type)}});
        result["bindings"] = std::move(bindings);
    }
    return result;
}

Json write_block(const std::vector<Instruction>& instructions)
{
    auto result = Json::array();
    for (const auto& instruction : instructions)
    {
        Json entry{{"op", instruction_name(instruction.kind)}, {"at", write_source(instruction.source)}};
        if (instruction.kind == Statement::assign || instruction.kind == Statement::for_) entry["slot"] = instruction.slot;
        if (instruction.first) entry["first"] = write_expression(*instruction.first);
        if (instruction.second) entry["second"] = write_expression(*instruction.second);
        if (!instruction.body.empty()) entry["body"] = write_block(instruction.body);
        if (!instruction.alternate.empty()) entry["alternate"] = write_block(instruction.alternate);
        if (!instruction.handlers.empty()) entry["handlers"] = write_block(instruction.handlers);
        if (instruction.kind == Statement::raise) entry["exception"] = instruction.exception;
        if (!instruction.host_sql.empty()) entry["sql"] = instruction.host_sql;
        if (!instruction.cursor.empty()) entry["cursor"] = instruction.cursor;
        if (!instruction.result_name.empty()) entry["result_name"] = instruction.result_name;
        if (!instruction.targets.empty()) entry["targets"] = instruction.targets;
        if (!instruction.bindings.empty())
        {
            auto bindings = Json::array();
            for (const auto& slot : instruction.bindings)
                bindings.push_back({{"name", slot.name}, {"slot", slot.slot}, {"type", write_type(slot.type)}});
            entry["bindings"] = std::move(bindings);
        }
        result.push_back(std::move(entry));
    }
    return result;
}
}

Program compile(const Routine& routine, uint64_t revision, const HostType& host)
{
    return Compiler(routine, revision, host).run();
}

Program compile_expression(std::span<const Variable> inputs, const SqlExpr& expression, const HostType& host)
{
    Routine routine{};
    routine.name = {{"decision_expression"}, expression.source};
    routine.variables.assign(inputs.begin(), inputs.end());
    return Compiler(routine, 1, host).run(&expression);
}

std::string serialize(const Program& program)
{
    Json slots = Json::array();
    for (const auto& slot : program.slots)
        slots.push_back({{"name", slot.name}, {"type", write_type(slot.type)}, {"mode", mode_name(slot.mode)},
            {"at", write_source(slot.source)}});
    Json record{
        {"ir_version", ir_version}, {"routine", program.name.parts}, {"at", write_source(program.name.source)},
        {"revision", std::to_string(program.revision)}, {"parameters", program.parameters},
        {"slots", std::move(slots)}, {"returns", program.returns ? write_type(*program.returns) : Json(nullptr)},
        {"body", write_block(program.body)}, {"handlers", write_block(program.handlers)},
        {"uses_sql", program.uses_sql}, {"deterministic", program.deterministic},
    };
    if (program.writes_sql) record["writes_sql"] = true;
    if (!program.cursors.empty()) record["cursors"] = program.cursors;
    if (!program.out_cursors.empty()) record["out_cursors"] = program.out_cursors;
    auto result = record.dump();
    if (result.size() > max_ir_bytes) throw Error(Code::limit, program.name.source, "Serialized PL/SQL IR exceeds its byte budget");
    return result;
}
}
