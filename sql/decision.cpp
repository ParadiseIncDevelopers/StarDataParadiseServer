#include "decision.hpp"
#include "decision_condition.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace sdp::sql
{
namespace
{
using wire::Json;
using W = wire::Code;
using Event = DecisionRule::Event;
using Effect = DecisionRule::Effect;
using Dependencies = std::map<ObjectId, uint32_t>;
constexpr size_t version_limit = 16;
constexpr size_t rules_per_table = 32;

struct Version
{
    std::string source;
    Dependencies dependencies;
};

struct Rule
{
    ObjectId target;
    uint32_t active;
    std::vector<Version> versions;
};

[[noreturn]] void damaged()
{
    throw disk::Error(disk::Fault::corrupt, "Invalid Decision rule catalog");
}

Rule read_rule(const Object& object)
{
    try
    {
        const auto value = Json::parse(object.definition);
        if (!value.is_object() || value.at("format") != 1 || !value.at("target").is_number_integer() ||
            !value.at("active").is_number_unsigned()) damaged();
        const auto id = value.at("target").get<ObjectId>();
        const auto active = value.at("active").get<uint64_t>();
        const auto& stored = value.at("versions");
        if (id <= 0 || !stored.is_array() || !active || active > stored.size() || stored.size() > version_limit)
            damaged();
        Rule result{id, static_cast<uint32_t>(active), {}};
        for (const auto& entry : stored)
        {
            const auto& dependencies = entry.at("dependencies");
            if (!dependencies.is_array() || dependencies.size() > 64) damaged();
            auto& version = result.versions.emplace_back(Version{entry.at("source").get<std::string>(), {}});
            for (const auto& ref : dependencies)
            {
                if (!ref.is_array() || ref.size() != 2 || !ref[0].is_number_integer() ||
                    !ref[1].is_number_unsigned()) damaged();
                const auto dependency = ref[0].get<ObjectId>();
                const auto stamp = ref[1].get<uint64_t>();
                if (dependency <= 0 || !stamp || stamp > UINT32_MAX ||
                    !version.dependencies.emplace(dependency, static_cast<uint32_t>(stamp)).second) damaged();
            }
            if (!version.dependencies.contains(id)) damaged();
        }
        return result;
    }
    catch (const Json::exception&) { damaged(); }
}

std::string write_rule(const Rule& rule)
{
    auto versions = Json::array();
    for (const auto& version : rule.versions)
    {
        auto references = Json::array();
        for (const auto [id, stamp] : version.dependencies) references.push_back(Json::array({id, stamp}));
        versions.push_back({{"source", version.source}, {"dependencies", std::move(references)}});
    }
    return Json{{"format", 1}, {"target", rule.target}, {"active", rule.active}, {"versions", std::move(versions)}}.dump();
}

const Object& require_object(std::span<const Object> objects, ObjectId id)
{
    const auto it = std::ranges::find(objects, id, &Object::id);
    if (it == objects.end()) throw Error(Code::name, {}, "A Decision rule dependency is missing");
    return *it;
}

uint32_t selected_version(const Object& object, const Rule& rule, const DecisionOverride& override)
{
    if (!override || override->first != object.id) return rule.active;
    if (!override->second || override->second > rule.versions.size())
        throw Error(Code::name, {}, "Unknown Decision rule version");
    return override->second;
}

const char* truth_name(Truth value)
{
    switch (value)
    {
    case Truth::true_: return "true";
    case Truth::false_: return "false";
    case Truth::unknown: return "unknown";
    }
    return "unknown";
}

const char* operator_name(Expr::Op op)
{
    using Op = Expr::Op;
    switch (op)
    {
    case Op::and_: return "and";
    case Op::or_: return "or";
    case Op::logical_not: return "not";
    case Op::eq: return "eq";
    case Op::ne: return "ne";
    case Op::lt: return "lt";
    case Op::le: return "le";
    case Op::gt: return "gt";
    case Op::ge: return "ge";
    case Op::is_null: return "is_null";
    case Op::is_not_null: return "is_not_null";
    case Op::exists: return "exists";
    case Op::routine_call: return "function";
    case Op::name: return "input";
    case Op::parameter: return "input";
    case Op::add: return "add";
    case Op::subtract: return "subtract";
    case Op::multiply: return "multiply";
    case Op::divide: return "divide";
    case Op::negate: return "negate";
    default: return "value";
    }
}

Json trace_node(const pl::ExpressionSample& sample)
{
    using State = pl::ExpressionState;
    return {{"id", sample.id}, {"parent_id", sample.parent_id}, {"op", operator_name(sample.op)},
        {"state", sample.state == State::evaluated ? "evaluated" : sample.state == State::skipped ? "skipped" : "failed"},
        {"truth", sample.truth ? Json(truth_name(*sample.truth)) : Json()}, {"elapsed_ns", sample.elapsed_ns}};
}

size_t assignment_index(const Object& table, const Assignment& assignment)
{
    if (assignment.column.parts.size() != 1)
        throw Error(Code::name, assignment.column.source, "Expected an unqualified Decision output column");
    const auto it = std::ranges::find(table.columns, assignment.column.parts.front(), &Column::name);
    if (it == table.columns.end())
        throw Error(Code::name, assignment.column.source, "Unknown Decision output column");
    return static_cast<size_t>(it - table.columns.begin());
}

Dependencies validate_rule(Catalog& catalog, const Object& table, const DecisionRule& rule,
    std::span<const Object> snapshot)
{
    if (!rule.condition) damaged();
    const auto condition = decision_type(catalog, table, *rule.condition);
    if (condition.kind != Kind::boolean && condition.kind != Kind::null)
        throw Error(Code::type, rule.condition->source, "A Decision condition must be BOOLEAN");
    Dependencies result{{table.id, table.version}};
    const auto collect = [&](const Expr& expression) {
        for (auto id : decision_dependencies(catalog, table, expression))
            result.insert_or_assign(id, require_object(snapshot, id).version);
    };
    collect(*rule.condition);
    if (rule.assignment)
    {
        if (rule.event == Event::delete_)
            throw Error(Code::type, rule.assignment->column.source, "DELETE has no Decision output row");
        const auto output = assignment_index(table, *rule.assignment);
        const auto type = decision_type(catalog, table, rule.assignment->value);
        if (!can_coerce(type, table.columns[output].type))
            throw Error(Code::type, rule.assignment->value.source, "Incompatible Decision output type");
        collect(rule.assignment->value);
    }
    if (result.size() > 64) throw Error(Code::limit, {}, "Decision dependency limit reached");
    return result;
}

DecisionRule parse_rule(const Version& rule)
{
    auto statement = parse(rule.source);
    if (statement.action != Action::create_decision_rule || !statement.decision) damaged();
    return std::move(*statement.decision);
}

void check_dependencies(std::span<const Object> snapshot, const Version& version)
{
    for (const auto [id, expected] : version.dependencies)
        if (require_object(snapshot, id).version != expected)
            throw Error(Code::constraint, {}, "Decision rule needs revalidation against the current catalog");
}

void check_cycles(std::span<const Object> objects, ObjectId changed_id, const Rule& replacement)
{
    std::map<ObjectId, std::set<ObjectId>> graph;
    const auto append = [&](const Rule& rule) {
        for (const auto [id, stamp] : rule.versions.at(rule.active - 1).dependencies)
            if (id != rule.target && require_object(objects, id).kind == ObjectKind::table)
                graph[rule.target].insert(id);
    };
    for (const auto& object : objects)
        if (object.kind == ObjectKind::decision && object.id != changed_id) append(read_rule(object));
    append(replacement);
    std::set<ObjectId> active, complete;
    const auto walk = [&](const auto& self, ObjectId id, size_t depth) -> void {
        if (complete.contains(id)) return;
        if (depth > 32 || !active.insert(id).second)
            throw Error(Code::constraint, {}, "Decision table dependency cycle or depth limit");
        if (const auto found = graph.find(id); found != graph.end())
            for (auto dependency : found->second) self(self, dependency, depth + 1);
        active.erase(id);
        complete.insert(id);
    };
    for (const auto& [id, ignored] : graph) walk(walk, id, 0);
}

bool applies(Event configured, Event event, Effect effect)
{
    return (configured == Event::write || configured == event) &&
        (event != Event::delete_ || effect != Effect::compute);
}
}

bool decision_ddl(Catalog& catalog, const Statement& statement, bool apply)
{
    if (statement.action != Action::create_decision_rule && statement.action != Action::activate_decision_rule)
        return false;
    if (!statement.decision || statement.related.parts.size() != 1) damaged();
    const auto& request = *statement.decision;
    const auto table = catalog.resolve(ObjectKind::table, statement.target);
    const auto snapshot = catalog.list();
    auto qualified = Catalog::qualified(table, snapshot);
    qualified.parts.back() = statement.related.parts.front();
    const auto known = std::ranges::find_if(snapshot, [&](const Object& object) {
        return object.parent == table.parent && object.name == qualified.parts.back();
    });
    if (known != snapshot.end() && known->kind != ObjectKind::decision)
        throw Error(Code::name, statement.source, "Decision rule name is already in use");

    Rule state = known == snapshot.end() ? Rule{table.id, 1, {}} : read_rule(*known);
    if (state.target != table.id) throw Error(Code::name, statement.source, "Decision rule belongs to another table");
    if (statement.action == Action::create_decision_rule)
    {
        if (state.versions.size() >= version_limit) throw Error(Code::limit, statement.source, "Decision version limit reached");
        if (known == snapshot.end())
        {
            size_t count{};
            for (const auto& object : snapshot)
                if (object.kind == ObjectKind::decision && read_rule(object).target == table.id) ++count;
            if (count >= rules_per_table) throw Error(Code::limit, statement.source, "Decision rule limit reached");
        }
        state.versions.push_back({request.definition, validate_rule(catalog, table, request, snapshot)});
    }
    else
    {
        if (!request.version || request.version > state.versions.size())
            throw Error(Code::name, statement.source, "Unknown Decision rule version");
        const auto& version = state.versions.at(request.version - 1);
        check_dependencies(snapshot, version);
        const auto rule = parse_rule(version);
        if (validate_rule(catalog, table, rule, snapshot) != version.dependencies)
            throw Error(Code::constraint, statement.source, "Decision rule dependencies changed");
        if (state.active == request.version) return true;
        state.active = request.version;
    }
    check_cycles(snapshot, known == snapshot.end() ? 0 : known->id, state);
    auto definition = write_rule(state);
    if (definition.size() > 16 * 1024)
        throw Error(Code::limit, statement.source, "Decision definition exceeds the catalog budget");
    if (known == snapshot.end()) catalog.validate_auxiliary(ObjectKind::decision, qualified);
    if (apply)
    {
        if (known == snapshot.end()) catalog.create_auxiliary(ObjectKind::decision, qualified, std::move(definition));
        else catalog.update_definition(*known, std::move(definition), true);
    }
    return true;
}

std::vector<ObjectId> decision_dependencies(const Object& object)
{
    if (object.kind != ObjectKind::decision) return {};
    std::set<ObjectId> references;
    for (const auto& version : read_rule(object).versions)
        for (const auto [id, stamp] : version.dependencies) references.insert(id);
    return {references.begin(), references.end()};
}

void check_decision_drop(std::span<const Object> objects, const Object& dropping)
{
    if (dropping.kind == ObjectKind::decision)
        throw Error(Code::constraint, {}, "A Decision rule cannot be removed");
    for (const auto& object : objects)
    {
        const auto targets = decision_dependencies(object);
        if (std::ranges::find(targets, dropping.id) != targets.end())
            throw Error(Code::constraint, {}, "Object is a Decision rule dependency");
    }
}

void require_decision_module(std::span<const Object> objects, ObjectId table, bool enabled)
{
    if (enabled) return;
    for (const auto& object : objects)
        if (object.kind == ObjectKind::decision && read_rule(object).target == table)
            throw DecisionError(W::module_unavailable, "module_unavailable");
}

ObjectId decision_rule_id(std::span<const Object> objects, ObjectId table, std::string_view name)
{
    for (const auto& object : objects)
        if (object.kind == ObjectKind::decision && object.name == name && read_rule(object).target == table)
            return object.id;
    throw Error(Code::name, {}, "Unknown Decision rule on target");
}

std::vector<ObjectId> decision_access_inputs(ObjectId table, std::span<const Object> objects,
    const DecisionOverride& version)
{
    std::set<ObjectId> inputs;
    for (const auto& object : objects)
    {
        if (object.kind != ObjectKind::decision) continue;
        const auto stored = read_rule(object);
        if (stored.target != table) continue;
        const auto& selected = stored.versions.at(selected_version(object, stored, version) - 1);
        for (const auto& dependency : selected.dependencies) inputs.insert(dependency.first);
    }
    return {inputs.begin(), inputs.end()};
}

std::vector<ObjectId> decision_inputs(Catalog& catalog, const Object& table,
    std::span<const Object> objects, const DecisionOverride& version)
{
    std::set<ObjectId> inputs;
    for (const auto& object : objects)
    {
        if (object.kind != ObjectKind::decision) continue;
        const auto stored = read_rule(object);
        if (stored.target != table.id) continue;
        const auto rule = parse_rule(stored.versions.at(selected_version(object, stored, version) - 1));
        for (const auto id : decision_dependencies(catalog, table, *rule.condition)) inputs.insert(id);
        if (rule.assignment)
            for (const auto id : decision_dependencies(catalog, table, rule.assignment->value)) inputs.insert(id);
    }
    return {inputs.begin(), inputs.end()};
}

void apply_decision(disk::Transaction& tx, disk::Owner owner, std::span<const Object> objects,
    const Object& table, const RowValues* before, RowValues* after, const WorkCheck& check, Json* trace,
    const DecisionOverride& override)
{
    struct Entry
    {
        std::string_view name;
        DecisionRule rule;
        ObjectId id;
        uint32_t version;
        size_t report_index{};
    };
    const auto event = !before ? Event::insert : !after ? Event::delete_ : Event::update;
    std::vector<Entry> active;
    try
    {
        for (const auto& object : objects)
        {
            if (object.kind != ObjectKind::decision) continue;
            if (check) check();
            const auto stored = read_rule(object);
            if (stored.target != table.id) continue;
            if (!tx.decision_enabled()) throw DecisionError(W::module_unavailable, "module_unavailable");
            const auto selected = selected_version(object, stored, override);
            const auto& version = stored.versions.at(selected - 1);
            auto rule = parse_rule(version);
            if (!applies(rule.event, event, rule.effect)) continue;
            check_dependencies(objects, version);
            active.push_back({object.name, std::move(rule), object.id, selected});
        }
        if (active.empty()) return;
        std::ranges::sort(active, [](const Entry& a, const Entry& b) {
            if (a.rule.priority != b.rule.priority) return a.rule.priority > b.rule.priority;
            return a.name < b.name;
        });
        if (trace)
            for (auto& entry : active)
            {
                entry.report_index = trace->size();
                trace->push_back({{"object_id", std::to_string(entry.id)}, {"version", entry.version},
                    {"state", "skipped"}, {"truth", nullptr}, {"elapsed_ns", 0}, {"nodes", Json::array()}});
            }
        const auto condition = [&](const Entry& entry, const RowValues* proposed) {
            const auto& rule = entry.rule;
            if (!trace) return truth(decision_value(tx, owner, table, *rule.condition, before, proposed, tx.actor(), check));
            auto& report = trace->at(entry.report_index);
            const auto start = std::chrono::steady_clock::now();
            auto evaluated = trace_decision_value(tx, owner, table, *rule.condition, before, proposed, tx.actor(), check);
            const auto result = truth(evaluated.value);
            report["state"] = "evaluated";
            report["truth"] = truth_name(result);
            report["elapsed_ns"] = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count();
            for (const auto& sample : evaluated.samples) report["nodes"].push_back(trace_node(sample));
            return result;
        };
        if (after)
        {
            auto calculated = *after;
            std::set<size_t> assigned;
            for (const auto& entry : active)
            {
                const auto& item = entry.rule;
                if (!item.assignment) continue;
                const auto output = assignment_index(table, *item.assignment);
                if (assigned.contains(output) || condition(entry, after) != Truth::true_) continue;
                const auto value = decision_value(tx, owner, table, item.assignment->value, before, after, tx.actor(), check);
                Catalog current(tx, owner, check);
                calculated[output] = coerce(value, decision_type(current, table, item.assignment->value), table.columns[output].type);
                assigned.insert(output);
            }
            *after = std::move(calculated);
        }
        bool has_guard = false;
        for (const auto& entry : active)
        {
            const auto& item = entry.rule;
            if (item.effect == Effect::compute) continue;
            has_guard = true;
            const auto match = condition(entry, after);
            if (match == Truth::unknown) throw DecisionError(W::permission_denied, "unknown");
            if (match != Truth::true_) continue;
            if (item.effect == Effect::allow) return;
            throw DecisionError(W::permission_denied, "denied");
        }
        if (has_guard) throw DecisionError(W::permission_denied, "denied");
    }
    catch (const Error&) { throw DecisionError(W::invalid_state, "policy_unavailable"); }
}
}
