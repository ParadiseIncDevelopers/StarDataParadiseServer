#include "decision_condition.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <string_view>

namespace
{
using namespace sdp;

template<class Sample>
concept CarriesRawValues = requires(Sample item) { item.value; } ||
    requires(Sample item) { item.operands; } || requires(Sample item) { item.literal; } ||
    requires(Sample item) { item.text; };
static_assert(!CarriesRawValues<pl::ExpressionSample>);

void require(bool passed, std::string_view detail)
{
    if (!passed) throw std::runtime_error(std::string(detail));
}

sql::Expr parse_expression(std::string_view expression)
{
    return sql::parse("SELECT " + std::string(expression)).projection.at(0);
}

void execute(sql::Catalog& catalog, disk::Transaction& transaction, std::string_view text)
{
    (void)sql::execute_write(transaction, 1, sql::bind(catalog, sql::parse(text)));
}

void verify_traces(disk::Transaction& tx, const sql::Object& table, const sql::RowValues& after)
{
    using Op = sql::Expr::Op;
    using State = pl::ExpressionState;
    const auto trace = [&](std::string_view expression) {
        auto result = sql::trace_decision_value(tx, 1, table, parse_expression(expression), nullptr, &after, "workbench");
        require(!result.samples.empty(), "Trace is missing its root expression");
        std::set<uint32_t> ids;
        for (auto item = result.samples.rbegin(); item != result.samples.rend(); ++item)
        {
            require(item->id && ids.insert(item->id).second &&
                (item->parent_id ? ids.contains(item->parent_id) : item == result.samples.rbegin()),
                "Trace tree lacks unique node identity or an observed parent");
            if (item->state == State::skipped)
                require(!item->truth && !item->elapsed_ns, "Skipped node acquired an evaluated value or time");
            if (item->parent_id && (item->op == Op::name || item->op == Op::boolean))
                require(!item->truth, "Trace exposed a Boolean leaf value");
        }
        return result;
    };
    const auto short_and = trace("FALSE AND (1 / 0 = 8)");
    const auto divided = std::ranges::find(short_and.samples, Op::divide, &pl::ExpressionSample::op);
    require(short_and.samples.back().op == Op::and_ && short_and.samples.back().truth == sql::Truth::false_ &&
        divided != short_and.samples.end() && divided->state == State::skipped,
        "Trace did not mark the unexecuted AND right branch");
    const auto short_or = trace("TRUE OR (1 / 0 = 8)");
    require(short_or.samples.back().truth == sql::Truth::true_ &&
        std::ranges::any_of(short_or.samples, [](const auto& s) { return s.op == Op::divide && s.state == State::skipped; }),
        "Trace did not mark the unexecuted OR right branch");
    const auto uncertain = trace("OLD.accepted AND TRUE");
    require(sql::truth(uncertain.value) == sql::Truth::unknown && uncertain.samples.back().truth == sql::Truth::unknown &&
        std::ranges::all_of(uncertain.samples, [](const auto& s) { return s.state == State::evaluated; }),
        "UNKNOWN did not evaluate both operands with SQL truth semantics");
    const auto pure = trace("positive(NEW.id)");
    require(sql::truth(pure.value) == sql::Truth::true_ && pure.samples.size() == 2 &&
        pure.samples.back().op == Op::routine_call && pure.samples.back().truth == sql::Truth::true_,
        "Scalar host call trace exposed internal routine expressions");

    std::vector<pl::ExpressionSample> failures;
    pl::Instrumentation instrumentation;
    instrumentation.expression = [&](const pl::ExpressionSample& sample) { failures.push_back(sample); };
    const auto failing = pl::compile_expression({}, parse_expression("1 / 0 = 8"));
    bool rejected{};
    try { (void)pl::invoke(failing, {}, {}, {}, {}, {}, instrumentation); }
    catch (const pl::RuntimeError&) { rejected = true; }
    require(rejected && std::ranges::any_of(failures,
        [](const auto& s) { return s.op == Op::divide && s.state == State::failed && !s.truth; }),
        "An executed error did not generate a failed-expression trace");
}

void checks(disk::Transaction& transaction, sql::Catalog& catalog)
{
    execute(catalog, transaction, "CREATE TABLE orders (id BIGINT PRIMARY KEY, accepted BOOLEAN)");
    execute(catalog, transaction, "CREATE TABLE payments (order_id BIGINT PRIMARY KEY, paid BOOLEAN)");
    execute(catalog, transaction, "CREATE FUNCTION positive(amount BIGINT) RETURN BOOLEAN AS BEGIN RETURN amount > 0; END;");
    const auto table = catalog.resolve(sql::ObjectKind::table, sql::parse_history_target("orders"));
    const auto payment = catalog.resolve(sql::ObjectKind::table, sql::parse_history_target("payments"));
    const auto function = catalog.resolve(sql::ObjectKind::routine, sql::parse_history_target("positive"));

    const sql::RowValues before{int64_t{12}, int64_t{0}};
    const sql::RowValues after{int64_t{12}, int64_t{1}};
    const auto value = [&](const sql::Expr& expression, const sql::RowValues* previous = nullptr) {
        return sql::decision_value(transaction, 1, table, expression, previous, &after, "workbench");
    };
    const auto predicate = parse_expression("NEW.accepted AND (NOT OLD.accepted) AND ACTOR = 'workbench'");
    require(sql::decision_type(catalog, table, predicate) == sql::Type{sql::Kind::boolean}, "Predicate binding changed its type");
    require(sql::truth(value(predicate, &before)) == sql::Truth::true_, "OLD/NEW/ACTOR were not bound from the write");
    require(sql::truth(value(predicate)) == sql::Truth::unknown, "Missing OLD row did not stay UNKNOWN");
    require(sql::truth(value(parse_expression("accepted AND positive(NEW.id)"))) == sql::Truth::true_,
        "Bare NEW column or verified pure PL/SQL function did not evaluate");
    require(sql::decision_dependencies(catalog, table, parse_expression("positive(NEW.id)")) == std::vector{function.id},
        "Scalar function dependency identity was omitted");
    require(std::get<int64_t>(value(parse_expression("NEW.id * 10 + 2"))) == 122,
        "Typed computation returned an incorrect value");

    const auto exists = parse_expression("EXISTS (SELECT 1 FROM payments WHERE order_id = :new.id AND paid = TRUE)");
    require(sql::decision_dependencies(catalog, table, exists) == std::vector{payment.id},
        "Cross-table dependency identity was omitted");
    require(sql::truth(value(exists)) == sql::Truth::false_, "Unpaid order unexpectedly found a payment");
    execute(catalog, transaction, "INSERT INTO payments VALUES (12,TRUE)");
    require(sql::truth(value(exists)) == sql::Truth::true_, "Decision EXISTS missed caller's same-transaction row");
    auto persisted_shape = exists;
    persisted_shape.query.reset();
    require(sql::truth(value(persisted_shape)) == sql::Truth::true_, "Persisted EXISTS text could not be rebound");
    verify_traces(transaction, table, after);

    execute(catalog, transaction, "CREATE FUNCTION hosted_check(amount BIGINT) RETURN BOOLEAN AS "
        "BEGIN RETURN EXISTS (SELECT 1 FROM payments WHERE order_id = :amount); END;");
    const auto rejects = [&](std::string_view source) {
        try { (void)value(parse_expression(source)); }
        catch (const sql::Error& error)
        {
            require(error.code() == sql::Code::unsupported, "Unsafe Decision returned the wrong error category");
            return;
        }
        throw std::runtime_error("Side-effect-capable Decision was permitted");
    };
    rejects("hosted_check(NEW.id)");
    rejects("EXISTS (SELECT positive(12))");
    rejects("EXISTS (SELECT NEXT VALUE FOR order_sequence)");
    require(sql::truth(value(exists)) == sql::Truth::true_, "Read-only Decision evaluation changed data");
    execute(catalog, transaction, "CREATE OR REPLACE FUNCTION positive(amount BIGINT) RETURN BOOLEAN AS "
        "BEGIN RETURN amount < 0; END;");
    require(sql::truth(value(parse_expression("positive(NEW.id)"))) == sql::Truth::false_,
        "Decision evaluated a stale routine definition");
}
}

int main(int count, char** arguments)
{
    if (count != 2) return 2;
    try
    {
        const auto root = std::filesystem::path(arguments[1]);
        std::filesystem::create_directories(root);
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto data = root / (std::to_string(stamp) + ".sdp");
        sdp::disk::Store::initialize(data);
        sdp::disk::Engine engine(data);
        auto transaction = engine.begin(1, std::chrono::milliseconds{0});
        sdp::sql::Catalog catalog(*transaction, 1);
        checks(*transaction, catalog);
        transaction->rollback(1);
        require(engine.current().lsn == 0, "Decision evaluation published a transaction");
        std::cout << "Decision conditions, scalar PL/SQL, snapshot EXISTS, read-only short circuit traces, and NULL passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
