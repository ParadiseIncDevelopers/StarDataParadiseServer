#include "binder.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <random>

namespace
{
using namespace sdp::sql;
using namespace std::chrono_literals;
constexpr sdp::disk::Owner owner = 42;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class Fn>
void expect(Code code, Fn action)
{
    try { action(); }
    catch (const Error& error)
    {
        require(error.code() == code, error.what());
        require(error.source().column && error.source().line, "Source position missing");
        return;
    }
    throw std::runtime_error("Expected SQL error");
}

Object ddl(Catalog& catalog, std::string_view text)
{
    const auto plan = check(catalog, text);
    return catalog.apply(std::get<Statement>(plan));
}

std::filesystem::path scratch_store(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path);
    const auto start = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int i = 0; i < 100; ++i)
    {
        auto directory = path / (std::to_string(start) + "-" + std::to_string(i));
        if (std::filesystem::create_directory(directory))
        {
            auto store = directory / "store.sdp";
            sdp::disk::Store::initialize(store);
            return store;
        }
    }
    throw std::runtime_error("Could not allocate isolated SQL test directory");
}

void catalog_test(const std::filesystem::path& file)
{
    using namespace sdp::disk;
    ObjectId table_id{};
    {
        Engine engine(file);
        auto transaction = engine.begin(owner, 0ms);
        Catalog catalog(*transaction, owner);
        ddl(catalog, "CREATE DATABASE archive");
        ddl(catalog, "CREATE SCHEMA archive.sales");
        const auto table = ddl(catalog, R"(CREATE TABLE archive.sales."Sipariş" (id BIGINT NOT NULL, "Tutar" DECIMAL(20,4)))");
        table_id = table.id;
        const auto renamed = catalog.resolve(ObjectKind::table, {{"archive", "sales", "Sipariş"}});
        require(renamed.id == table_id && renamed.columns.size() == 2 && renamed.version == 1, "Catalog not visible in transaction");
        const auto point = transaction->savepoint(owner);
        ddl(catalog, R"(ALTER TABLE archive.sales."Sipariş" ADD COLUMN status TEXT)");
        transaction->rollback_to(owner, point);
        require(catalog.resolve(ObjectKind::table, {{"archive", "sales", "Sipariş"}}).columns.size() == 2,
            "DDL savepoint did not undo column");
        expect(Code::name, [&] { ddl(catalog, "CREATE TABLE bad (x BIGINT, x TEXT)"); });
        expect(Code::name, [&] { catalog.resolve(ObjectKind::table, {{"bad"}}); });
        require(transaction->commit(owner) == 1, "Initial catalog commit missing");
        const auto history = engine.history();
        require(history.size() == 1 && history.front().changes.size() == 3, "Catalog changes missing from atomic WAL");
        for (const auto& change : history.front().changes)
            require(change.kind == ChangeKind::catalog && change.before.empty() && !change.after.empty(), "Invalid catalog journal change");
    }
    {
        Engine engine(file);
        auto transaction = engine.begin(owner, 0ms);
        Catalog catalog(*transaction, owner);
        auto object = catalog.resolve(ObjectKind::table, {{"archive", "sales", "Sipariş"}});
        require(object.id == table_id && object.version == 1 && object.columns[1].name == "Tutar",
            "Catalog ID or definition changed after reopen");
        expect(Code::constraint, [&] { ddl(catalog, "DROP SCHEMA archive.sales"); });
        ddl(catalog, R"(DROP TABLE archive.sales."Sipariş")");
        transaction->rollback(owner);
        require(engine.history().size() == 1, "Rolled-back catalog changes became durable");
        transaction = engine.begin(owner, 0ms);
        Catalog replacement(*transaction, owner);
        ddl(replacement, R"(DROP TABLE archive.sales."Sipariş")");
        const auto next = ddl(replacement, R"(CREATE TABLE archive.sales."Sipariş" (id BIGINT))");
        require(next.id != table_id, "A dropped table ID was reused");
        ddl(replacement, R"(ALTER TABLE archive.sales."Sipariş" ADD COLUMN status TEXT)");
        require(replacement.resolve(ObjectKind::table, {{"archive", "sales", "Sipariş"}}).version == 2,
            "Schema version did not advance");
        transaction->commit(owner);
    }
    {
        sdp::disk::Store store(file, true);
        Catalog readonly(store);
        require(readonly.resolve(ObjectKind::table, {{"archive", "sales", "Sipariş"}}).version == 2,
            "Read-only metadata missed the new schema");
        expect(Code::unsupported, [&] { readonly.apply(parse("CREATE TABLE nope (id BIGINT)")); });
    }
}

void type_test(const std::filesystem::path& file)
{
    sdp::disk::Store store(file, true);
    Catalog catalog(store);
    const auto projection = [&](std::string_view source) {
        auto plan = std::get<SelectPlan>(check(catalog, source));
        std::vector<sdp::disk::Value> values;
        for (const auto& column : plan.projection) values.push_back(evaluate(column.expression));
        return values;
    };
    const auto truth_table = projection("SELECT NULL AND TRUE, NULL AND FALSE, NULL OR FALSE, "
        "NULL OR TRUE, NOT NULL, NULL = NULL, NULL IS NULL, TRUE = FALSE");
    const std::vector<sdp::disk::Value> expected{{}, int64_t{0}, {}, int64_t{1}, {}, {}, int64_t{1}, int64_t{0}};
    require(truth_table == expected, "NULL truth table differs");
    require(!passes_filter(std::get<SelectPlan>(check(catalog, "SELECT 1 WHERE NULL"))),
        "Unknown predicate qualified a row");
    require(projection("SELECT -9223372036854775808, 4 = 4.0, "
        "TIMESTAMP '2024-02-29T13:00:00+03:00' = TIMESTAMP '2024-02-29T10:00:00Z'") ==
        std::vector<sdp::disk::Value>{INT64_MIN, int64_t{1}, int64_t{1}}, "Exact comparisons failed");

    expect(Code::range, [&] { check(catalog, "SELECT 9223372036854775808"); });
    expect(Code::range, [&] { parse_literal({Kind::decimal, 4, 2}, "999.99"); });
    expect(Code::type, [&] { check(catalog, "SELECT DATE '2024-02-29' = '2024-02-29'"); });
    expect(Code::type, [&] { check(catalog, "SELECT 1 WHERE 'true'"); });
    require(parse_literal({Kind::decimal, 4, 2}, "-12.345") ==
        sdp::disk::Value{sdp::disk::Decimal{"-1235", 2}}, "Decimal half rounding failed");
    require(parse_literal({Kind::text}, "İstanbul") == sdp::disk::Value{std::string("İstanbul")}, "UTF-8 lost");
}

void decision_parser_test()
{
    using Event = DecisionRule::Event;
    using Effect = DecisionRule::Effect;
    using Op = Expr::Op;
    constexpr std::string_view definition =
        "/* guard */ CREATE DECISION RULE \"Approve\" ON sales.orders FOR UPDATE PRIORITY 255 "
        "WHEN (NEW.status = 'approved' AND NOT EXISTS (SELECT 1 FROM payments p WHERE p.order_id = OLD.id)) THEN DENY;";
    const auto guard = parse(definition);
    require(guard.action == Action::create_decision_rule && guard.target.parts == std::vector<std::string>{"sales", "orders"} &&
        guard.related.parts == std::vector<std::string>{"Approve"} && guard.decision &&
        guard.decision->event == Event::update && guard.decision->effect == Effect::deny &&
        guard.decision->priority == 255 && guard.decision->condition && !guard.decision->assignment &&
        guard.decision->definition == definition,
        "Decision guard identity or source was lost");
    const auto& condition = *guard.decision->condition;
    require(condition.op == Op::and_ && condition.args.at(0).args.at(0).name.parts.at(0) == "new" &&
        condition.args.at(1).op == Op::logical_not && condition.args.at(1).args.at(0).op == Op::exists &&
        condition.args.at(1).args.at(0).query &&
        condition.args.at(1).args.at(0).query->target.parts.at(0) == "payments",
        "Decision guard expression or EXISTS source was lost");

    const auto compute = parse("CREATE DECISION RULE total ON orders FOR WRITE PRIORITY 0 WHEN (TRUE) "
        "THEN SET \"Total\" = NEW.quantity * NEW.price");
    require(compute.decision && compute.decision->event == Event::write && compute.decision->effect == Effect::compute &&
        compute.decision->priority == 0 && compute.decision->assignment &&
        compute.decision->assignment->column.parts.at(0) == "Total" &&
        compute.decision->assignment->value.op == Op::multiply, "Decision assignment AST was lost");
    const auto activate = parse("ALTER DECISION RULE total ON orders ACTIVATE VERSION 4294967295");
    require(activate.action == Action::activate_decision_rule && activate.decision &&
        activate.decision->version == UINT32_MAX && !activate.decision->condition, "Decision version AST was lost");
    require(parse("CREATE TABLE decision (rule BIGINT, priority BIGINT, allow BOOLEAN, version BIGINT)").columns.size() == 4,
        "Contextual Decision keywords changed existing identifiers");

    expect(Code::range, [] { parse("ALTER DECISION RULE r ON t ACTIVATE VERSION 0"); });
    expect(Code::range, [] { parse("ALTER DECISION RULE r ON t ACTIVATE VERSION 4294967296"); });
    expect(Code::range, [] { parse("CREATE DECISION RULE r ON t FOR INSERT PRIORITY 4294967296 WHEN (TRUE) THEN ALLOW"); });
    expect(Code::syntax, [] { parse("CREATE DECISION RULE r ON t FOR READ PRIORITY 1 WHEN (TRUE) THEN ALLOW"); });
    expect(Code::syntax, [] { parse("CREATE DECISION RULE r ON t FOR DELETE PRIORITY 1 WHEN (TRUE) THEN SKIP"); });
    expect(Code::syntax, [] { parse("CREATE DECISION RULE r ON t FOR DELETE PRIORITY 1 WHEN TRUE THEN DENY"); });
    expect(Code::syntax, [] { parse("CREATE DECISION RULE r ON t FOR WRITE PRIORITY 1 WHEN (TRUE) THEN SET NEW.value=0"); });
    expect(Code::unsupported, [] { parse("CREATE DECISION RULE r ON t FOR WRITE PRIORITY 1 WHEN (TRUE) THEN DENY; SELECT 1"); });

    using Inspection = DecisionInspection::Kind;
    constexpr std::array inspections{
        std::pair{"EXPLAIN", Inspection::explain}, std::pair{"WHYNOT", Inspection::whynot},
        std::pair{"WHATIF", Inspection::whatif}, std::pair{"TEST", Inspection::test},
        std::pair{"RECHECK", Inspection::recheck}, std::pair{"VALIDATE", Inspection::validate}};
    for (const auto& [verb, kind] : inspections)
    {
        const auto command = parse(std::string("DECISION ") + verb + " USING '{\"text\":\"İzmir''in\"}';");
        require(command.action == Action::decision_inspect && command.decision_inspection &&
            command.decision_inspection->kind == kind && command.decision_inspection->body == R"({"text":"İzmir'in"})",
            "Decision inspection changed its operation or quoted JSON bytes");
    }
    require(parse("SELECT whatif, whynot, recheck, validate FROM decision").projection.size() == 4,
        "Contextual inspection keywords changed existing identifiers");
    expect(Code::syntax, [] { parse("DECISION REPORT USING '{}'"); });
    expect(Code::syntax, [] { parse("DECISION VALIDATE '{}'"); });
    expect(Code::syntax, [] { parse("DECISION EXPLAIN USING 1"); });
    expect(Code::unsupported, [] { parse("DECISION WHATIF USING '{}'; SELECT 1"); });
}

void parser_test(const std::filesystem::path& file)
{
    sdp::disk::Engine engine(file);
    auto transaction = engine.begin(owner, 0ms);
    Catalog catalog(*transaction, owner);
    ddl(catalog, R"(CREATE TABLE sources (k BIGINT, "I" TEXT, "İ" TEXT))");
    auto plan = std::get<SelectPlan>(check(catalog, R"(SELECT t."I", t."İ" FROM sources t WHERE t.k = :k)",
        {{"k", {Kind::integer}}}));
    std::vector<sdp::disk::Value> row{int64_t{7}, std::string("I"), std::string("İ")};
    require(passes_filter(plan, row, {{"k", int64_t{7}}}) && !passes_filter(plan, row, {{"k", int64_t{6}}}),
        "Parameter was captured as a column");
    require(evaluate(plan.projection[1].expression, row) == row[2], "Quoted name resolution is not exact");
    expect(Code::name, [&] { check(catalog, "SELECT sources.k FROM sources t"); });
    expect(Code::name, [&] { check(catalog, "SELECT :k FROM sources"); });
    expect(Code::unsupported, [&] { check(catalog, "MERGE INTO sources"); });
    expect(Code::unsupported, [&] { check(catalog, "SELECT * FROM sources INTERSECT SELECT * FROM sources"); });
    try
    {
        check(catalog, "SELECT k FROM sources\nWHERE )");
        throw std::runtime_error("Expected source position");
    }
    catch (const Error& error)
    {
        require(error.source().line == 2 && error.source().column >= 6, "Multiline error position incorrect");
    }
    expect(Code::limit, [] { parse(std::string(65537, 'A')); });
    decision_parser_test();
    std::mt19937 random(0xFED9);
    for (size_t trial = 0; trial < 2500; ++trial)
    {
        std::string bytes;
        const size_t len = random() % 97;
        for (size_t i = 0; i < len; ++i) bytes.push_back(static_cast<char>(random() % 256));
        try { (void)parse(bytes); }
        catch (const Error&) {}
    }
}
}

int main(int argc, char* argv[])
{
    try
    {
        if (argc != 3) return 2;
        const auto file = scratch_store(argv[2]);
        const std::string_view test(argv[1]);
        if (test == "catalog") catalog_test(file);
        else if (test == "types") type_test(file);
        else if (test == "parser") parser_test(file);
        else return 2;
        std::cout << test << " passed\n";
        return 0;
    }
    catch (const Error& e)
    {
        std::cerr << e.source().line << ':' << e.source().column << ' ' << e.what() << '\n';
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; }
    return 1;
}
