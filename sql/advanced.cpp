#include "advanced.hpp"

#include "collation.hpp"
#include "definition.hpp"

#include <algorithm>
#include <charconv>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

namespace sdp::sql
{
namespace
{
using Op = Expr::Op;
using Rows = std::vector<RowValues>;
using PlanPtr = std::shared_ptr<const AdvancedPlan>;
using Ctes = std::map<std::string, PlanPtr, std::less<>>;
constexpr size_t max_relations = 16;
constexpr size_t max_slots = 512;
constexpr size_t max_plan_nodes = 64;
constexpr size_t cardinality_limit = 1'000'000;
constexpr auto boolean_type = Type{Kind::boolean};
constexpr auto integer_type = Type{Kind::integer};

struct Relation
{
    std::optional<Object> table;
    PlanPtr query;
    std::vector<size_t> source_columns;
    std::vector<std::string> qualifier;
    size_t offset{};
    size_t estimate{};
};

struct Slot
{
    std::string name;
    Type type;
    size_t relation;
};

struct JoinStep
{
    JoinClause::Kind kind;
    std::optional<BoundExpression> condition;
    std::optional<std::pair<size_t, size_t>> hash_columns;
    size_t estimate;
};

enum class AggregateKind { count, sum, avg, minimum, maximum };
struct Aggregate
{
    AggregateKind kind;
    std::optional<BoundExpression> argument;
    Type type;
    Source source;
    size_t slot;
};

struct ScalarQuery
{
    PlanPtr query;
    bool exists;
    size_t slot;
    Source source;
};

struct Order
{
    BoundExpression expression;
    bool descending;
    std::string collation;
};

struct SetArm
{
    bool all;
    PlanPtr query;
};

BoundExpression column(size_t index, Type type, Source source = {})
{
    BoundExpression result;
    result.op = Op::name;
    result.type = type;
    result.column = index;
    result.source = source;
    return result;
}

Type literal_type(const Expr& expr)
{
    switch (expr.op)
    {
    case Op::null: return {Kind::null};
    case Op::boolean: return boolean_type;
    case Op::integer: return integer_type;
    case Op::decimal: return decimal_literal_type(expr.text, expr.source);
    case Op::text: return {Kind::text};
    case Op::date: return {Kind::date};
    case Op::time: return {Kind::time};
    case Op::timestamp_tz: return {Kind::timestamp_tz};
    default: throw Error(Code::unsupported, expr.source, "Unsupported scalar expression");
    }
}

void require_boolean(const BoundExpression& expr)
{
    if (!can_coerce(expr.type, boolean_type)) throw Error(Code::type, expr.source, "Expected a boolean predicate");
}

bool same_expression(const BoundExpression& left, const BoundExpression& right)
{
    return left.op == right.op && left.type == right.type && left.literal == right.literal &&
        left.column == right.column && left.parameter == right.parameter &&
        std::ranges::equal(left.operands, right.operands, same_expression);
}

bool outer_slot(const BoundExpression& expression, size_t first_excluded)
{
    if (expression.op == Op::name && expression.column >= first_excluded) return true;
    return std::ranges::any_of(expression.operands, [&](const auto& operand) {
        return outer_slot(operand, first_excluded);
    });
}

void find_pushable(const BoundExpression& expression, size_t base_width, std::vector<BoundExpression>& pushed)
{
    if (expression.op == Op::and_)
    {
        for (const auto& child : expression.operands) find_pushable(child, base_width, pushed);
    }
    else if (!outer_slot(expression, base_width))
        pushed.push_back(expression);
}

Type union_type(Type left, Type right, Source source)
{
    if (left.kind == Kind::null) return right;
    if (right.kind == Kind::null || left == right) return left;
    const auto numeric = [](Kind kind) { return kind == Kind::integer || kind == Kind::decimal; };
    if (numeric(left.kind) && numeric(right.kind)) return {Kind::decimal, 38, (std::max)(left.scale, right.scale)};
    throw Error(Code::type, source, "UNION columns require compatible types");
}

size_t product(size_t left, size_t right)
{
    if (left && right > cardinality_limit / left) return cardinality_limit;
    return left * right;
}

size_t estimate_rows(const Object& table)
{
    constexpr size_t fallback = 64;
    if (table.definition.empty()) return fallback;
    try
    {
        const auto definition = wire::Json::parse(table.definition);
        if (!definition.contains("statistics")) return fallback;
        const auto& statistics = definition.at("statistics");
        if (statistics.at("schema_version") != table.version || !statistics.at("rows").is_number_unsigned())
            return fallback;
        return (std::min)(cardinality_limit, statistics.at("rows").get<size_t>());
    }
    catch (const wire::Json::exception&) { return fallback; }
}
}

struct AdvancedPlan
{
    std::vector<Slot> slots;
    std::vector<Relation> relations;
    std::vector<JoinStep> joins;
    std::vector<BoundExpression> pushed;
    std::optional<BoundExpression> filter;
    std::vector<BoundExpression> grouping;
    std::optional<BoundExpression> having;
    std::vector<Aggregate> aggregates;
    std::vector<ScalarQuery> subqueries;
    std::vector<Projection> core_columns;
    std::vector<Projection> columns;
    std::vector<Order> order;
    std::vector<SetArm> unions;
    size_t synthetic_count{};
    size_t estimate{1};
    bool distinct{};
    bool grouped{};
};

namespace
{
class Planner
{
public:
    Planner(Catalog& catalog, const ParameterTypes& params, size_t& count)
        : catalog_(catalog), params_(params), count_(count) {}

    PlanPtr build(const Statement& statement, Ctes ctes = {}, size_t depth = 0)
    {
        if (++count_ > max_plan_nodes || depth >= 16)
            throw Error(Code::limit, statement.source, "Query plan nesting limit exceeded");
        if (statement.action != Action::select) throw Error(Code::unsupported, statement.source, "Expected SELECT");

        auto plan = std::make_shared<AdvancedPlan>();
        for (const auto& cte : statement.ctes)
        {
            if (ctes.contains(cte.name)) throw Error(Code::name, statement.source, "Duplicate CTE name");
            ctes.emplace(cte.name, build(*cte.query, ctes, depth + 1));
        }
        if (!statement.target.parts.empty()) add_relation(*plan, statement.target, statement.alias, ctes);
        for (const auto& item : statement.joins) add_relation(*plan, item.target, item.alias, ctes);

        const auto width = plan->slots.size();
        const auto bind = [&](const Expr& expr, bool aggregates, size_t visible) {
            return expression(*plan, expr, ctes, depth, aggregates, visible);
        };
        size_t estimate = plan->relations.empty() ? 1 : plan->relations.front().estimate;
        for (size_t index = 0; index < statement.joins.size(); ++index)
        {
            const auto& right = plan->relations[index + 1];
            const auto end = index + 2 == plan->relations.size() ? width : plan->relations[index + 2].offset;
            const auto& input = statement.joins[index];
            JoinStep join{input.kind, {}, {}, 0};
            if (input.on)
            {
                join.condition = bind(*input.on, false, end);
                require_boolean(*join.condition);
                if (right.estimate >= 8) join.hash_columns = hash_key(*join.condition, right.offset, end);
            }
            const auto cardinality = product(estimate, right.estimate);
            join.estimate = join.hash_columns ? cardinality / (std::max)(size_t{1}, (std::max)(estimate, right.estimate))
                                              : join.condition ? cardinality / 3 : cardinality;
            if (join.kind == JoinClause::Kind::left) join.estimate = (std::max)(estimate, join.estimate);
            estimate = join.estimate;
            plan->joins.push_back(std::move(join));
        }
        if (statement.filter)
        {
            plan->filter = bind(*statement.filter, false, width);
            require_boolean(*plan->filter);
            if (!plan->relations.empty())
                find_pushable(*plan->filter, plan->relations.size() > 1 ? plan->relations[1].offset : width, plan->pushed);
            estimate /= 3;
        }
        for (const auto& group : statement.group_by) plan->grouping.push_back(bind(group, false, width));
        if (statement.having)
        {
            plan->having = bind(*statement.having, true, width);
            require_boolean(*plan->having);
        }
        if (statement.all_columns)
        {
            if (!width) throw Error(Code::name, statement.source, "SELECT * requires a table");
            for (size_t index = 0; index < width; ++index)
                plan->core_columns.push_back({plan->slots[index].name, column(index, plan->slots[index].type)});
        }
        else
        {
            for (const auto& item : statement.projection)
            {
                auto bound = bind(item, true, width);
                auto name = item.op == Op::name ? item.name.parts.back() :
                    "column" + std::to_string(plan->core_columns.size() + 1);
                plan->core_columns.push_back({std::move(name), std::move(bound)});
            }
        }
        plan->columns = plan->core_columns;

        for (const auto& arm : statement.unions)
        {
            auto query = build(*arm.query, ctes, depth + 1);
            if (query->columns.size() != plan->columns.size())
                throw Error(Code::type, statement.source, "UNION column counts differ");
            for (size_t index = 0; index < plan->columns.size(); ++index)
            {
                auto& type = plan->columns[index].expression.type;
                type = union_type(type, query->columns[index].expression.type, statement.source);
            }
            plan->unions.push_back({arm.all, std::move(query)});
        }
        for (const auto& term : statement.order_by)
        {
            auto bound = order_expression(*plan, term.expression, [&] { return bind(term.expression, true, width); });
            auto collation = std::string(canonical_collation(term.collation));
            if (!term.collation.empty() && bound.type.kind != Kind::text && bound.type.kind != Kind::null)
                throw Error(Code::type, bound.source, "COLLATE requires TEXT");
            plan->order.push_back({std::move(bound), term.descending, std::move(collation)});
        }
        if (statement.distinct && plan->unions.empty())
            for (auto& term : plan->order)
            {
                const auto projection = std::ranges::find_if(plan->core_columns, [&](const Projection& item) {
                    return same_expression(term.expression, item.expression);
                });
                if (projection == plan->core_columns.end())
                    throw Error(Code::unsupported, term.expression.source, "SELECT DISTINCT must order by projected values");
                term.expression = column(static_cast<size_t>(projection - plan->core_columns.begin()),
                    term.expression.type, term.expression.source);
            }
        plan->grouped = !plan->grouping.empty() || !plan->aggregates.empty() || plan->having.has_value();
        if (plan->grouped)
        {
            if (plan->having) require_grouped(*plan, *plan->having);
            for (const auto& projection : plan->core_columns) require_grouped(*plan, projection.expression);
            if (plan->unions.empty() && !statement.distinct)
                for (const auto& term : plan->order) require_grouped(*plan, term.expression);
            estimate = plan->grouping.empty() ? 1 : estimate / 3;
        }
        for (const auto& arm : plan->unions)
            estimate = (std::min)(cardinality_limit, estimate + arm.query->estimate);
        plan->estimate = estimate;
        plan->distinct = statement.distinct;
        return plan;
    }

private:
    void add_relation(AdvancedPlan& plan, const Name& name, const std::optional<std::string>& alias, const Ctes& ctes)
    {
        if (plan.relations.size() == max_relations) throw Error(Code::limit, name.source, "JOIN relation limit exceeded");
        Relation relation;
        relation.offset = plan.slots.size();
        const auto cte = name.parts.size() == 1 ? ctes.find(name.parts.front()) : ctes.end();
        if (cte != ctes.end())
        {
            relation.query = cte->second;
            relation.qualifier = {alias.value_or(name.parts.front())};
            relation.estimate = relation.query->estimate;
            for (const auto& projected : relation.query->columns)
                plan.slots.push_back({projected.column, projected.expression.type, plan.relations.size()});
        }
        else
        {
            auto object = catalog_.resolve_relation(name);
            relation.qualifier = alias ? std::vector<std::string>{*alias} : catalog_.qualified(object).parts;
            if (object.kind == ObjectKind::view)
            {
                auto view = read_view(catalog_, object);
                relation.table = std::move(view.table);
                relation.source_columns = std::move(view.columns);
            }
            else
            {
                relation.table = object;
                for (size_t i = 0; i < object.columns.size(); ++i) relation.source_columns.push_back(i);
            }
            relation.estimate = estimate_rows(*relation.table);
            for (const auto index : relation.source_columns)
            {
                const auto& field = relation.table->columns.at(index);
                plan.slots.push_back({field.name, field.type, plan.relations.size()});
            }
        }
        if (plan.slots.size() > max_slots) throw Error(Code::limit, name.source, "Query column limit exceeded");
        for (const auto& previous : plan.relations)
            if (relation.qualifier == previous.qualifier)
                throw Error(Code::name, name.source, "Repeated relation requires a distinct alias");
        plan.relations.push_back(std::move(relation));
    }

    static std::optional<std::pair<size_t, size_t>> hash_key(const BoundExpression& expr, size_t right_offset, size_t end)
    {
        if (expr.op != Op::eq) return {};
        auto* left = &expr.operands.at(0);
        auto* right = &expr.operands.at(1);
        if (left->op != Op::name || right->op != Op::name || left->type != right->type ||
            left->type.kind == Kind::timestamp_tz || left->type.kind == Kind::null)
            return {};
        if (left->column >= right_offset) std::swap(left, right);
        if (left->column >= right_offset || right->column < right_offset || right->column >= end) return {};
        return std::pair{left->column, right->column - right_offset};
    }

    BoundExpression name(const AdvancedPlan& plan, const Expr& item, size_t visible) const
    {
        const auto& parts = item.name.parts;
        const auto prefix = parts.size() - 1;
        std::optional<size_t> found;
        for (size_t index = 0; index < visible; ++index)
        {
            const auto& slot = plan.slots[index];
            const auto& qualifier = plan.relations[slot.relation].qualifier;
            if (slot.name != parts.back() || prefix > qualifier.size() ||
                !std::equal(parts.begin(), parts.begin() + prefix, qualifier.end() - prefix))
                continue;
            if (found) throw Error(Code::name, item.source, "Ambiguous column reference");
            found = index;
        }
        if (!found) throw Error(Code::name, item.source, "Unknown column in query scope");
        return column(*found, plan.slots[*found].type, item.source);
    }

    BoundExpression expression(AdvancedPlan& plan, const Expr& expr, const Ctes& ctes, size_t depth, bool aggregating,
        size_t visible)
    {
        if (expr.op == Op::name) return name(plan, expr, visible);
        if (expr.op == Op::aggregate)
        {
            if (!aggregating) throw Error(Code::type, expr.source, "Aggregate is unavailable in this clause");
            Aggregate aggregate{AggregateKind::count, {}, integer_type, expr.source, 0};
            if (!expr.args.empty()) aggregate.argument = expression(plan, expr.args.front(), ctes, depth, false, visible);
            if (expr.text == "sum" || expr.text == "avg")
            {
                if (!aggregate.argument || (aggregate.argument->type.kind != Kind::integer &&
                    aggregate.argument->type.kind != Kind::decimal && aggregate.argument->type.kind != Kind::null))
                    throw Error(Code::type, expr.source, "SUM/AVG requires a number");
                aggregate.kind = expr.text == "sum" ? AggregateKind::sum : AggregateKind::avg;
                const auto scale = aggregate.argument->type.scale;
                aggregate.type = {Kind::decimal, 38,
                    aggregate.kind == AggregateKind::sum ? scale : (std::max)(scale, uint8_t{6})};
            }
            else if (expr.text == "min" || expr.text == "max")
            {
                if (!aggregate.argument) throw Error(Code::type, expr.source, "MIN/MAX requires a value");
                aggregate.kind = expr.text == "min" ? AggregateKind::minimum : AggregateKind::maximum;
                aggregate.type = aggregate.argument->type;
            }
            else if (expr.text != "count")
                throw Error(Code::unsupported, expr.source, "Aggregate function is unsupported");
            const auto previous = std::ranges::find_if(plan.aggregates, [&](const Aggregate& item) {
                return item.kind == aggregate.kind && item.argument.has_value() == aggregate.argument.has_value() &&
                    (!item.argument || same_expression(*item.argument, *aggregate.argument));
            });
            if (previous != plan.aggregates.end()) return column(previous->slot, previous->type, expr.source);
            aggregate.slot = plan.slots.size() + plan.synthetic_count++;
            const auto result = column(aggregate.slot, aggregate.type, expr.source);
            plan.aggregates.push_back(std::move(aggregate));
            return result;
        }
        if (expr.op == Op::exists || expr.op == Op::subquery)
        {
            auto subquery = build(*expr.query, ctes, depth + 1);
            const bool exists = expr.op == Op::exists;
            if (!exists && subquery->columns.size() != 1)
                throw Error(Code::type, expr.source, "Scalar subquery requires one output column");
            const auto type = exists ? boolean_type : subquery->columns.front().expression.type;
            const auto slot = plan.slots.size() + plan.synthetic_count++;
            plan.subqueries.push_back({std::move(subquery), exists, slot, expr.source});
            return column(slot, type, expr.source);
        }

        BoundExpression bound;
        bound.op = expr.op;
        bound.source = expr.source;
        if (expr.op == Op::parameter)
        {
            const auto value = params_.find(expr.text);
            if (value == params_.end()) throw Error(Code::name, expr.source, "Parameter type is missing");
            validate_type(value->second, expr.source);
            bound.type = value->second;
            bound.parameter = expr.text;
            return bound;
        }
        if (expr.op <= Op::timestamp_tz)
        {
            bound.type = literal_type(expr);
            bound.literal = parse_literal(bound.type, expr.text, expr.source);
            return bound;
        }

        for (const auto& item : expr.args) bound.operands.push_back(expression(plan, item, ctes, depth, aggregating, visible));
        bound.type = boolean_type;
        switch (expr.op)
        {
        case Op::logical_not:
        case Op::and_:
        case Op::or_:
            for (const auto& operand : bound.operands) require_boolean(operand);
            break;
        case Op::is_null:
        case Op::is_not_null:
            break;
        case Op::negate:
            bound.type = bound.operands.at(0).type;
            if (bound.type.kind != Kind::null && bound.type.kind != Kind::integer && bound.type.kind != Kind::decimal)
                throw Error(Code::type, expr.source, "Unary minus requires a number");
            break;
        case Op::eq: case Op::ne: case Op::lt: case Op::le: case Op::gt: case Op::ge:
            if (!comparable(bound.operands.at(0).type, bound.operands.at(1).type))
                throw Error(Code::type, expr.source, "Comparison operands have incompatible types");
            break;
        default:
            throw Error(Code::unsupported, expr.source, "Unsupported SELECT expression");
        }
        return bound;
    }

    static BoundExpression order_expression(const AdvancedPlan& plan, const Expr& expr,
        const std::function<BoundExpression()>& bind)
    {
        std::optional<size_t> position;
        if (expr.op == Op::integer)
        {
            size_t value{};
            const auto parsed = std::from_chars(expr.text.data(), expr.text.data() + expr.text.size(), value);
            if (parsed.ec != std::errc{} || !value || value > plan.columns.size())
                throw Error(Code::name, expr.source, "ORDER BY position is outside the projection");
            position = value - 1;
        }
        else if (!plan.unions.empty() && expr.op == Op::name && expr.name.parts.size() == 1)
        {
            for (size_t index = 0; index < plan.columns.size(); ++index)
                if (plan.columns[index].column == expr.name.parts.front())
                {
                    if (position) throw Error(Code::name, expr.source, "Ambiguous ORDER BY output name");
                    position = index;
                }
        }
        if (!plan.unions.empty())
        {
            if (!position) throw Error(Code::unsupported, expr.source, "UNION orders by output names or positions");
            return column(*position, plan.columns[*position].expression.type, expr.source);
        }
        return position ? plan.core_columns[*position].expression : bind();
    }

    static void require_grouped(const AdvancedPlan& plan, const BoundExpression& expression)
    {
        if (std::ranges::any_of(plan.grouping, [&](const auto& group) { return same_expression(group, expression); })) return;
        if (expression.op == Op::name && expression.column < plan.slots.size())
            throw Error(Code::type, expression.source, "Column requires GROUP BY or an aggregate");
        for (const auto& child : expression.operands) require_grouped(plan, child);
    }

    Catalog& catalog_;
    const ParameterTypes& params_;
    size_t& count_;
};

bool is_null(const disk::Value& value) { return std::holds_alternative<std::monostate>(value); }

int compare(const disk::Value& left, const disk::Value& right, Type type, std::string_view collation = {})
{
    if (is_null(left)) return is_null(right) ? 0 : -1;
    if (is_null(right)) return 1;
    if (type.kind == Kind::text && !collation.empty())
        return compare_collated(std::get<std::string>(left), std::get<std::string>(right), collation);
    return compare_non_null(left, type, right, type);
}

struct RowLess
{
    std::vector<Type> types;
    bool operator()(const RowValues& left, const RowValues& right) const
    {
        for (size_t index = 0; index < types.size(); ++index)
        {
            const auto order = compare(left[index], right[index], types[index]);
            if (order) return order < 0;
        }
        return false;
    }
};

size_t weight(const disk::Value& value)
{
    constexpr auto base = sizeof(disk::Value);
    if (const auto* bytes = std::get_if<disk::Bytes>(&value)) return base + bytes->capacity();
    if (const auto* text = std::get_if<std::string>(&value)) return base + text->capacity();
    if (const auto* number = std::get_if<disk::Decimal>(&value)) return base + number->unscaled.capacity();
    return base;
}

size_t weight(const RowValues& row)
{
    size_t bytes = sizeof(RowValues) * 2 + row.capacity() * sizeof(disk::Value);
    for (const auto& value : row) bytes += weight(value) - sizeof(disk::Value);
    return bytes;
}

class Memory
{
public:
    explicit Memory(size_t limit) : limit_(limit) {}
    void add(size_t bytes)
    {
        if (bytes > limit_ - used_) throw Error(Code::limit, {}, "Query memory budget exceeded");
        used_ += bytes;
    }
    void release(size_t bytes) noexcept { used_ -= bytes; }

private:
    size_t used_{};
    size_t limit_;
};

class Reservation
{
public:
    explicit Reservation(Memory& memory) : memory_(&memory) {}
    Reservation(Reservation&& peer) noexcept : memory_(peer.memory_), bytes_(std::exchange(peer.bytes_, 0)) {}
    Reservation& operator=(Reservation&& peer) noexcept
    {
        clear();
        memory_ = peer.memory_;
        bytes_ = std::exchange(peer.bytes_, 0);
        return *this;
    }
    ~Reservation() { clear(); }
    void add(size_t bytes)
    {
        memory_->add(bytes);
        bytes_ += bytes;
    }
    void remove(size_t bytes) noexcept
    {
        memory_->release(bytes);
        bytes_ -= bytes;
    }
    void clear() noexcept { remove(bytes_); }

private:
    Memory* memory_;
    size_t bytes_{};
};

struct Data
{
    explicit Data(Memory& memory) : reserved(memory) {}
    void push(RowValues row)
    {
        reserved.add(weight(row));
        rows.push_back(std::move(row));
    }
    void clear()
    {
        Rows().swap(rows);
        reserved.clear();
    }
    Reservation reserved;
    Rows rows;
};

struct Magnitude
{
    std::string digits{"0"};
    bool negative{};
};

int order_digits(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) return left.size() < right.size() ? -1 : 1;
    return left.compare(right);
}

std::string add_digits(std::string_view left, std::string_view right)
{
    std::string result;
    int carry{};
    while (!left.empty() || !right.empty() || carry)
    {
        const auto a = left.empty() ? 0 : left.back() - '0';
        const auto b = right.empty() ? 0 : right.back() - '0';
        if (!left.empty()) left.remove_suffix(1);
        if (!right.empty()) right.remove_suffix(1);
        const auto digit = a + b + carry;
        result.push_back(static_cast<char>('0' + digit % 10));
        carry = digit / 10;
    }
    std::ranges::reverse(result);
    return result;
}

std::string subtract_digits(std::string_view left, std::string_view right)
{
    std::string result;
    int borrow{};
    while (!left.empty())
    {
        auto digit = left.back() - '0' - borrow - (right.empty() ? 0 : right.back() - '0');
        left.remove_suffix(1);
        if (!right.empty()) right.remove_suffix(1);
        borrow = digit < 0;
        if (borrow) digit += 10;
        result.push_back(static_cast<char>('0' + digit));
    }
    while (result.size() > 1 && result.back() == '0') result.pop_back();
    std::ranges::reverse(result);
    return result;
}

void accumulate(Magnitude& sum, const disk::Value& value)
{
    auto text = std::holds_alternative<int64_t>(value)
        ? std::to_string(std::get<int64_t>(value)) : std::get<disk::Decimal>(value).unscaled;
    const auto negative = text.front() == '-';
    const auto digits = std::string_view(text).substr(negative ? 1 : 0);
    if (sum.negative == negative)
        sum.digits = add_digits(sum.digits, digits);
    else if (order_digits(sum.digits, digits) >= 0)
        sum.digits = subtract_digits(sum.digits, digits);
    else
    {
        sum.digits = subtract_digits(digits, sum.digits);
        sum.negative = negative;
    }
    if (sum.digits == "0") sum.negative = false;
}

std::string divide_rounded(std::string_view digits, uint64_t divisor)
{
    std::string result;
    uint64_t remainder{};
    for (const auto digit : digits)
    {
        remainder = remainder * 10 + static_cast<unsigned>(digit - '0');
        const auto quotient = remainder / divisor;
        if (!result.empty() || quotient) result.push_back(static_cast<char>('0' + quotient));
        remainder %= divisor;
    }
    if (result.empty()) result = "0";
    if (remainder >= (divisor + 1) / 2) result = add_digits(result, "1");
    return result;
}

struct AggregateState
{
    size_t count{};
    disk::Value extreme;
    Magnitude sum;
};

struct Group
{
    RowValues representative;
    std::vector<AggregateState> states;
};

struct Output
{
    explicit Output(Memory& memory) : data(memory), order_keys(memory) {}
    Data data;
    Data order_keys;
};

class Runner
{
public:
    Runner(disk::Transaction& transaction, disk::Owner owner, const ParameterValues& parameters,
        const WorkCheck& check, size_t memory_limit)
        : transaction_(transaction), owner_(owner), parameters_(parameters), check_(check), memory_(memory_limit) {}

    Data execute(const AdvancedPlan& plan)
    {
        poll();
        RowValues scalars(plan.synthetic_count);
        Reservation scalar_claim(memory_);
        scalar_claim.add(weight(scalars));
        for (const auto& scalar : plan.subqueries)
        {
            const auto result = execute(*scalar.query);
            if (!scalar.exists && result.rows.size() > 1)
                throw Error(Code::constraint, scalar.source, "Scalar subquery returned more than one row");
            auto value = scalar.exists ? disk::Value{int64_t{!result.rows.empty()}} :
                result.rows.empty() ? disk::Value{} : result.rows.front().front();
            scalar_claim.add(weight(value));
            scalars[scalar.slot - plan.slots.size()] = std::move(value);
        }

        auto input = select_input(plan, scalars);
        Output output(memory_);
        const bool post_order = plan.distinct || !plan.unions.empty();
        if (plan.grouped)
            grouped(plan, input, scalars, output, post_order);
        else
            for (const auto& source : input.rows)
            {
                poll();
                if (accepts(plan.filter, plan, source, scalars))
                    emit(plan, source, scalars, output, post_order);
            }
        input.clear();

        auto types = result_types(plan.core_columns);
        if (plan.distinct) deduplicate(output.data, types);
        if (!plan.unions.empty())
        {
            const auto final_types = result_types(plan.columns);
            convert(output.data, types, final_types);
            for (const auto& arm : plan.unions)
            {
                auto extra = execute(*arm.query);
                convert(extra, result_types(arm.query->columns), final_types);
                for (auto& row : extra.rows)
                {
                    poll();
                    output.data.push(std::move(row));
                }
                extra.clear();
                if (!arm.all) deduplicate(output.data, final_types);
            }
        }
        if (!plan.order.empty())
        {
            if (post_order)
                for (const auto& row : output.data.rows)
                {
                    poll();
                    RowValues keys;
                    for (const auto& term : plan.order) keys.push_back(evaluate(term.expression, row, parameters_));
                    output.order_keys.push(std::move(keys));
                }
            sort(plan.order, output);
        }
        return std::move(output.data);
    }

private:
    void poll() const { if (check_) check_(); }

    Data source(const AdvancedPlan& plan, const Relation& relation, bool push)
    {
        if (relation.query)
        {
            auto rows = execute(*relation.query);
            if (push)
                std::erase_if(rows.rows, [&](const RowValues& row) { return !passes_push(plan, row); });
            return rows;
        }
        Data rows(memory_);
        if (!relation.table) return rows;
        scan_table(transaction_, owner_, *relation.table, [&](disk::RowId, const RowValues& physical) {
            RowValues visible;
            visible.reserve(relation.source_columns.size());
            for (const auto index : relation.source_columns) visible.push_back(physical.at(index));
            if (!push || passes_push(plan, visible)) rows.push(std::move(visible));
            return true;
        }, check_);
        return rows;
    }

    bool passes_push(const AdvancedPlan& plan, const RowValues& row)
    {
        poll();
        return std::ranges::all_of(plan.pushed, [&](const BoundExpression& expr) {
            return truth(evaluate(expr, row, parameters_), expr.source) == Truth::true_;
        });
    }

    RowValues frame(const AdvancedPlan& plan, const RowValues& row, const RowValues& scalars)
    {
        RowValues values(row);
        values.resize(plan.slots.size());
        values.insert(values.end(), scalars.begin(), scalars.end());
        return values;
    }

    bool accepts(const std::optional<BoundExpression>& expr, const AdvancedPlan& plan, const RowValues& row,
        const RowValues& scalars)
    {
        if (!expr) return true;
        if (scalars.empty()) return truth(evaluate(*expr, row, parameters_), expr->source) == Truth::true_;
        auto values = frame(plan, row, scalars);
        Reservation scratch(memory_);
        scratch.add(weight(values));
        return truth(evaluate(*expr, values, parameters_), expr->source) == Truth::true_;
    }

    static std::string hash_value(const disk::Value& value)
    {
        const auto bytes = disk::encode({1, {value}});
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    Data select_input(const AdvancedPlan& plan, const RowValues& scalars)
    {
        Data rows(memory_);
        if (plan.relations.empty())
        {
            rows.push({});
            return rows;
        }
        rows = source(plan, plan.relations.front(), true);
        for (size_t i = 0; i < plan.joins.size(); ++i)
        {
            const auto& step = plan.joins[i];
            const auto& relation = plan.relations[i + 1];
            const auto width = (i + 2 < plan.relations.size() ? plan.relations[i + 2].offset : plan.slots.size())
                - relation.offset;
            auto right = source(plan, relation, false);
            Data next(memory_);
            Reservation hash_claim(memory_);
            std::unordered_map<std::string, std::vector<size_t>> hash;
            if (step.hash_columns)
            {
                const auto key = step.hash_columns->second;
                for (size_t rid = 0; rid < right.rows.size(); ++rid)
                {
                    poll();
                    const auto& value = right.rows[rid].at(key);
                    if (is_null(value)) continue;
                    auto bytes = hash_value(value);
                    if (!hash.contains(bytes))
                        hash_claim.add(bytes.size() + sizeof(std::string) + sizeof(std::vector<size_t>) + 64);
                    hash_claim.add(sizeof(size_t) * 2);
                    hash[std::move(bytes)].push_back(rid);
                }
            }

            for (const auto& left : rows.rows)
            {
                poll();
                bool matched{};
                const auto probe = [&](const RowValues& right_row) {
                    poll();
                    RowValues joined(left);
                    joined.insert(joined.end(), right_row.begin(), right_row.end());
                    if (!accepts(step.condition, plan, joined, scalars)) return;
                    next.push(std::move(joined));
                    matched = true;
                };
                if (step.hash_columns)
                {
                    const auto& key = left.at(step.hash_columns->first);
                    if (!is_null(key))
                        if (const auto bucket = hash.find(hash_value(key)); bucket != hash.end())
                            for (const auto rid : bucket->second) probe(right.rows[rid]);
                }
                else for (const auto& right_row : right.rows) probe(right_row);
                if (!matched && step.kind == JoinClause::Kind::left)
                {
                    RowValues joined(left);
                    joined.resize(left.size() + width);
                    next.push(std::move(joined));
                }
            }
            rows = std::move(next);
        }
        return rows;
    }

    static std::vector<Type> result_types(std::span<const Projection> columns)
    {
        std::vector<Type> types;
        for (const auto& column : columns) types.push_back(column.expression.type);
        return types;
    }

    void add_aggregate(const Aggregate& aggregate, AggregateState& state, const RowValues& input, Reservation& claim)
    {
        if (!aggregate.argument)
        {
            ++state.count;
            return;
        }
        auto value = evaluate(*aggregate.argument, input, parameters_);
        if (is_null(value)) return;
        ++state.count;
        if (aggregate.kind == AggregateKind::sum || aggregate.kind == AggregateKind::avg)
            accumulate(state.sum, value);
        if (aggregate.kind == AggregateKind::minimum || aggregate.kind == AggregateKind::maximum)
        {
            const auto order = compare(value, state.extreme, aggregate.type);
            if (state.count == 1 || (aggregate.kind == AggregateKind::minimum ? order < 0 : order > 0))
            {
                claim.add(weight(value));
                claim.remove(weight(state.extreme));
                state.extreme = std::move(value);
            }
        }
    }

    static disk::Value aggregate_value(const Aggregate& aggregate, const AggregateState& state)
    {
        if (aggregate.kind == AggregateKind::count) return static_cast<int64_t>(state.count);
        if (!state.count) return {};
        if (aggregate.kind == AggregateKind::minimum || aggregate.kind == AggregateKind::maximum) return state.extreme;
        auto digits = state.sum.digits;
        if (aggregate.kind == AggregateKind::avg)
        {
            digits.append(aggregate.type.scale - aggregate.argument->type.scale, '0');
            digits = divide_rounded(digits, state.count);
        }
        if (state.sum.negative && digits != "0") digits.insert(digits.begin(), '-');
        return coerce(disk::Decimal{std::move(digits), aggregate.type.scale}, aggregate.type, aggregate.type, aggregate.source);
    }

    void grouped(const AdvancedPlan& plan, Data& input, const RowValues& scalars, Output& output, bool post_order)
    {
        std::vector<Type> types;
        for (const auto& expr : plan.grouping) types.push_back(expr.type);
        std::map<RowValues, Group, RowLess> groups(RowLess{std::move(types)});
        Reservation claim(memory_);
        const auto add_group = [&](RowValues key, const RowValues& source) -> Group& {
            const auto found = groups.find(key);
            if (found != groups.end()) return found->second;
            claim.add(weight(key) + weight(source) + 96 +
                plan.aggregates.size() * (sizeof(AggregateState) + sizeof(disk::Value) + 80));
            return groups.emplace(std::move(key), Group{source, std::vector<AggregateState>(plan.aggregates.size())}).first->second;
        };
        if (plan.grouping.empty()) add_group({}, {});
        for (const auto& row : input.rows)
        {
            poll();
            auto values = frame(plan, row, scalars);
            Reservation scratch(memory_);
            scratch.add(weight(values));
            if (plan.filter && truth(evaluate(*plan.filter, values, parameters_), plan.filter->source) != Truth::true_)
                continue;
            RowValues key;
            for (const auto& expr : plan.grouping) key.push_back(evaluate(expr, values, parameters_));
            auto& group = add_group(std::move(key), row);
            for (size_t index = 0; index < plan.aggregates.size(); ++index)
                add_aggregate(plan.aggregates[index], group.states[index], values, claim);
        }
        input.clear();
        for (const auto& [key, group] : groups)
        {
            poll();
            auto values = frame(plan, group.representative, scalars);
            for (size_t index = 0; index < plan.aggregates.size(); ++index)
                values[plan.aggregates[index].slot] = aggregate_value(plan.aggregates[index], group.states[index]);
            Reservation scratch(memory_);
            scratch.add(weight(values));
            if (plan.having && truth(evaluate(*plan.having, values, parameters_), plan.having->source) != Truth::true_)
                continue;
            emit_frame(plan, values, output, post_order);
        }
    }

    void emit(const AdvancedPlan& plan, const RowValues& row, const RowValues& scalars, Output& output, bool post_order)
    {
        if (scalars.empty()) emit_frame(plan, row, output, post_order);
        else
        {
            auto values = frame(plan, row, scalars);
            Reservation scratch(memory_);
            scratch.add(weight(values));
            emit_frame(plan, values, output, post_order);
        }
    }

    void emit_frame(const AdvancedPlan& plan, const RowValues& values, Output& output, bool post_order)
    {
        if (output.data.rows.size() == cardinality_limit)
            throw Error(Code::limit, {}, "Query result row budget exceeded");
        Reservation scratch(memory_);
        RowValues row;
        for (const auto& projected : plan.core_columns)
        {
            auto value = evaluate(projected.expression, values, parameters_);
            scratch.add(weight(value));
            row.push_back(std::move(value));
        }
        scratch.clear();
        output.data.push(std::move(row));
        if (post_order || plan.order.empty()) return;
        RowValues keys;
        for (const auto& term : plan.order) keys.push_back(evaluate(term.expression, values, parameters_));
        output.order_keys.push(std::move(keys));
    }

    void deduplicate(Data& output, const std::vector<Type>& types)
    {
        const RowLess less{types};
        std::ranges::sort(output.rows, [&](const auto& left, const auto& right) {
            poll();
            return less(left, right);
        });
        const auto tail = std::ranges::unique(output.rows, [&](const auto& left, const auto& right) {
            return !less(left, right) && !less(right, left);
        });
        output.rows.erase(tail.begin(), tail.end());
    }

    void convert(Data& data, const std::vector<Type>& from, const std::vector<Type>& to)
    {
        if (from == to) return;
        for (auto& row : data.rows)
        {
            poll();
            for (size_t index = 0; index < row.size(); ++index)
            {
                if (from[index] == to[index]) continue;
                auto value = coerce(row[index], from[index], to[index]);
                data.reserved.add(weight(value));
                data.reserved.remove(weight(row[index]));
                row[index] = std::move(value);
            }
        }
    }

    void sort(const std::vector<Order>& order, Output& output)
    {
        const auto& keys = output.order_keys.rows;
        Reservation scratch(memory_);
        scratch.add(output.data.rows.size() * (sizeof(size_t) + sizeof(RowValues)));
        std::vector<size_t> indexes;
        indexes.reserve(keys.size());
        for (size_t index = 0; index < keys.size(); ++index) indexes.push_back(index);
        std::ranges::sort(indexes, [&](size_t left, size_t right) {
            poll();
            for (size_t i = 0; i < order.size(); ++i)
            {
                const auto& left_key = keys[left][i];
                const auto& right_key = keys[right][i];
                Reservation conversion(memory_);
                if (order[i].collation != "binary" && !is_null(left_key) && !is_null(right_key))
                {
                    const auto bytes = std::get<std::string>(left_key).size() + std::get<std::string>(right_key).size();
                    conversion.add((bytes + 2) * sizeof(wchar_t));
                }
                const auto cmp = compare(left_key, right_key, order[i].expression.type, order[i].collation);
                if (cmp) return order[i].descending ? cmp > 0 : cmp < 0;
            }
            return left < right;
        });
        Rows sorted;
        sorted.reserve(indexes.size());
        for (const auto index : indexes) sorted.push_back(std::move(output.data.rows[index]));
        output.data.rows.swap(sorted);
    }

    disk::Transaction& transaction_;
    disk::Owner owner_;
    const ParameterValues& parameters_;
    const WorkCheck& check_;
    Memory memory_;
};

bool advanced_expr(const Expr& expression)
{
    return expression.op == Op::aggregate || expression.op == Op::exists || expression.op == Op::subquery ||
        std::ranges::any_of(expression.args, advanced_expr);
}

wire::Json explain_relation(const Relation& relation)
{
    if (relation.query)
        return {{"operator", "cte_scan"}, {"estimated_rows", relation.estimate}, {"input", explain_advanced(*relation.query)}};
    return {{"operator", "table_scan"}, {"estimated_rows", relation.estimate}, {"object_id", relation.table->id}};
}
}

bool advanced_select(const Statement& statement)
{
    if (statement.action != Action::select) return false;
    return statement.distinct || !statement.joins.empty() || !statement.ctes.empty() || !statement.group_by.empty() ||
        statement.having.has_value() || !statement.order_by.empty() || !statement.unions.empty() ||
        std::ranges::any_of(statement.projection, advanced_expr) ||
        (statement.filter && advanced_expr(*statement.filter));
}

std::shared_ptr<const AdvancedPlan> plan_advanced(Catalog& catalog, const Statement& statement,
    const ParameterTypes& parameters)
{
    size_t count{};
    return Planner(catalog, parameters, count).build(statement);
}

const std::vector<Projection>& advanced_columns(const AdvancedPlan& plan) { return plan.columns; }

std::vector<RowValues> run_advanced(const AdvancedPlan& plan, disk::Transaction& transaction, disk::Owner owner,
    const ParameterValues& parameters, const WorkCheck& check, size_t memory_limit, size_t row_limit)
{
    if (!memory_limit || memory_limit > 32 * 1024 * 1024 || !row_limit || row_limit > cardinality_limit)
        throw Error(Code::limit, {}, "Invalid advanced query budget");
    transaction.ensure_active(owner);
    Runner runner(transaction, owner, parameters, check, memory_limit);
    auto result = runner.execute(plan);
    if (result.rows.size() > row_limit) result.rows.resize(row_limit);
    return std::move(result.rows);
}

wire::Json explain_advanced(const AdvancedPlan& plan)
{
    using wire::Json;
    Json node = plan.relations.empty() ? Json{{"operator", "constant_row"}, {"estimated_rows", 1}} :
        explain_relation(plan.relations.front());
    if (!plan.pushed.empty()) node = {{"operator", "filter"}, {"pushdown", true}, {"input", std::move(node)}};
    for (size_t index = 0; index < plan.joins.size(); ++index)
    {
        const auto& join = plan.joins[index];
        const auto kind = join.kind == JoinClause::Kind::left ? "left" :
            join.kind == JoinClause::Kind::cross ? "cross" : "inner";
        node = {{"operator", join.hash_columns ? "hash_join" : "nested_loop_join"}, {"kind", kind},
            {"estimated_rows", join.estimate}, {"left", std::move(node)}, {"right", explain_relation(plan.relations[index + 1])}};
    }
    if (plan.filter) node = {{"operator", "filter"}, {"input", std::move(node)}};
    if (plan.grouped) node = {{"operator", "aggregate"}, {"group_keys", plan.grouping.size()}, {"input", std::move(node)}};
    if (plan.having) node = {{"operator", "having"}, {"input", std::move(node)}};
    node = {{"operator", "project"}, {"columns", plan.core_columns.size()}, {"input", std::move(node)}};
    if (plan.distinct) node = {{"operator", "distinct"}, {"input", std::move(node)}};
    for (const auto& arm : plan.unions)
        node = {{"operator", arm.all ? "union_all" : "union"}, {"left", std::move(node)}, {"right", explain_advanced(*arm.query)}};
    if (!plan.order.empty()) node = {{"operator", "sort"}, {"keys", plan.order.size()}, {"input", std::move(node)}};
    if (!plan.subqueries.empty())
    {
        Json nodes = Json::array();
        for (const auto& scalar : plan.subqueries)
            nodes.push_back({{"operator", scalar.exists ? "exists" : "scalar_subquery"}, {"input", explain_advanced(*scalar.query)}});
        node = {{"operator", "uncorrelated_subqueries"}, {"queries", std::move(nodes)}, {"input", std::move(node)}};
    }
    node["estimated_rows"] = plan.estimate;
    return node;
}
}
