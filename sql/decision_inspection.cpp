#include "decision_inspection.hpp"
#include "history_query.hpp"
#include "wire_values.hpp"

#include <algorithm>
#include <charconv>

namespace sdp::sql
{
namespace
{
using wire::Json;
using W = wire::Code;

[[noreturn]] void unavailable()
{
    throw DecisionError(W::module_unavailable, "dependency_snapshot_unavailable");
}

[[noreturn]] void invalid() { throw wire::Error(W::invalid_argument); }

void check_fields(const Json& input, std::initializer_list<std::string_view> allowed)
{
    if (!input.is_object()) invalid();
    for (const auto& item : input.items())
        if (std::ranges::find(allowed, item.key()) == allowed.end()) invalid();
}

uint64_t identifier(const Json& input, const char* name)
{
    if (!input.contains(name)) return 0;
    const auto& value = wire::string_field(input, name);
    uint64_t result{};
    if (value.empty() || value.front() == '0') invalid();
    const auto* end = value.data() + value.size();
    const auto parsed = std::from_chars(value.data(), end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end || !result) invalid();
    return result;
}

const Object* by_id(std::span<const Object> objects, ObjectId id)
{
    const auto found = std::ranges::find(objects, id, &Object::id);
    return found != objects.end() ? &*found : nullptr;
}

void need(const Security& security, const Principal& actor, std::span<const Object> objects,
    const Object& object, std::string_view verb)
{
    const auto required = object_access(objects, object, verb);
    enforce(security, actor, {&required, 1});
}

void ensure_frame(const Json& result)
{
    if (result.dump().size() > wire::max_payload - 4096) throw wire::Error(W::resource_limit);
}

class Inspection
{
public:
    Inspection(disk::Engine& engine, disk::Transaction& tx, disk::Owner owner, const Security& sec,
        const Principal& who, const Json& input, const WorkCheck& check)
        : tx_(tx), owner_(owner), security_(sec), actor_(who), check_(check), catalog_(tx, owner, check),
          current_(catalog_.list()), head_(engine.current().lsn)
    {
        const auto name = parse_history_target(wire::string_field(input, "target"));
        const auto* active = history_target(current_, name);
        if (!active || active->kind != ObjectKind::table) throw wire::Error(W::permission_denied);
        need(security_, actor_, current_, *active, "select");
        auto revision = identifier(input, "revision");
        if (revision)
        {
            need(security_, actor_, current_, *active, "history");
            HistoryJournal journal(engine, tx, owner, check);
            historic_ = std::make_unique<HistoryView>(journal, tx, owner, check);
            if (revision < historic_->state().floor || revision > head_) unavailable();
            past_ = historic_->catalog_at(revision);
            table_ = by_id(past_, active->id);
            if (!table_ || table_->kind != ObjectKind::table || table_->version != active->version) unavailable();
        }
        else
            table_ = active;
        revision_ = revision ? revision : head_;
    }

    Json run(const Json& body, const DecisionOverride& override = {})
    {
        check_();
        const auto& event = wire::string_field(body, "event");
        const auto row_id = identifier(body, "row_id");
        if (event == "insert")
        {
            if (row_id || !body.contains("proposed")) invalid();
        }
        else if (event == "update" || event == "delete")
        {
            if (!row_id || (event == "delete" && body.contains("proposed"))) invalid();
        }
        else invalid();

        for (auto id : decision_access_inputs(table_->id, objects(), override))
        {
            const auto* live = by_id(current_, id);
            if (!live)
            {
                if (actor_.administrator && historic_) unavailable();
                throw wire::Error(W::permission_denied);
            }
            const auto verb = live->kind == ObjectKind::routine ? "execute" : "select";
            need(security_, actor_, current_, *live, verb);
            if (historic_)
            {
                const auto* old = by_id(past_, id);
                if (!old || old->version != live->version || old->definition != live->definition) unavailable();
            }
        }
        if (historic_ && revision_ != head_)
        {
            try
            {
                for (const auto id : decision_inputs(catalog_, *table_, past_, override))
                {
                    const auto* object = by_id(current_, id);
                    if (!object) unavailable();
                    if (object->kind != ObjectKind::table) continue;
                    check_();
                    if (historic_->rows_at(id, revision_) != historic_->rows_at(id, head_)) unavailable();
                }
            }
            catch (const Error&) { unavailable(); }
        }

        std::optional<RowValues> before, after;
        if (row_id) before = row_at(row_id);
        if (event != "delete")
        {
            after = before.value_or(RowValues(table_->columns.size()));
            if (body.contains("proposed")) patch(*after, wire::object_field(body, "proposed"));
        }

        const auto proposed = after;
        auto trace = Json::array();
        bool allowed = true;
        std::string_view reason = "allowed";
        try
        {
            apply_decision(tx_, owner_, objects(), *table_, before ? &*before : nullptr,
                after ? &*after : nullptr, check_, &trace, override);
        }
        catch (const DecisionError& error)
        {
            if (error.code() != W::permission_denied) throw;
            allowed = false;
            reason = error.reason();
        }
        auto computed = Json::object();
        if (allowed && after)
            for (size_t i = 0; i < after->size(); ++i)
                if ((*proposed)[i] != (*after)[i])
                {
                    const auto& column = table_->columns[i];
                    computed[column.name] = write_value((*after)[i], column.type);
                }
        Json result{{"revision", std::to_string(revision_)}, {"allowed", allowed}, {"reason", reason},
            {"rules", std::move(trace)}, {"computed", std::move(computed)}};
        ensure_frame(result);
        return result;
    }

    Json compare(const Json& input)
    {
        const auto name = parse_history_target(wire::string_field(input, "rule"));
        if (name.parts.size() != 1) invalid();
        const auto id = decision_rule_id(objects(), table_->id, name.parts.front());
        const auto old_version = unsigned_field(input, "from_version", UINT32_MAX);
        const auto new_version = unsigned_field(input, "to_version", UINT32_MAX);
        if (!old_version || !new_version || old_version == new_version) invalid();
        const auto& cases = input.at("cases");
        if (!cases.is_array() || cases.empty() || cases.size() > 16) invalid();
        auto outcomes = Json::array();
        size_t changed{};
        size_t bytes{};
        for (const auto& example : cases)
        {
            check_fields(example, {"event", "row_id", "proposed"});
            auto prior = run(example, std::pair{id, old_version});
            auto candidate = run(example, std::pair{id, new_version});
            if (prior["allowed"] != candidate["allowed"] || prior["computed"] != candidate["computed"] ||
                prior["reason"] != candidate["reason"]) ++changed;
            auto result = Json{{"prior", std::move(prior)}, {"candidate", std::move(candidate)}};
            bytes += result.dump().size() + 1;
            if (bytes > wire::max_payload - 4096) throw wire::Error(W::resource_limit);
            outcomes.push_back(std::move(result));
        }
        return {{"revision", std::to_string(revision_)}, {"changed", changed}, {"cases", std::move(outcomes)}};
    }

private:
    std::span<const Object> objects() const { return historic_ ? past_ : current_; }

    RowValues row_at(disk::RowId id)
    {
        if (historic_)
        {
            const auto rows = historic_->rows_at(table_->id, revision_, id);
            if (const auto found = rows.find(id); found != rows.end())
                return read_history_values(*table_, found->second);
            unavailable();
        }
        try
        {
            const auto raw = tx_.get(owner_, id);
            const auto record = history_record(raw);
            if (record && record->table == table_->id) return read_history_values(*table_, raw);
        }
        catch (const disk::Error& error)
        {
            if (error.fault() != disk::Fault::missing) throw;
        }
        throw wire::Error(W::invalid_state);
    }

    void patch(RowValues& values, const Json& proposed)
    {
        ParameterTypes types;
        for (const auto& item : proposed.items())
        {
            const auto column = std::ranges::find(table_->columns, item.key(), &Column::name);
            if (column == table_->columns.end()) invalid();
            types.emplace(column->name, column->type);
        }
        const auto supplied = read_parameters(proposed, types);
        for (size_t i = 0; i < table_->columns.size(); ++i)
            if (const auto value = supplied.find(table_->columns[i].name); value != supplied.end())
                values[i] = value->second;
    }

    disk::Transaction& tx_;
    disk::Owner owner_;
    const Security& security_;
    const Principal& actor_;
    const WorkCheck& check_;
    Catalog catalog_;
    std::vector<Object> current_, past_;
    const Object* table_{};
    const uint64_t head_;
    uint64_t revision_{};
    std::unique_ptr<HistoryView> historic_;
};
}

Json inspect_decision(disk::Engine& engine, disk::Transaction& tx, disk::Owner owner,
    const Security& security, const Principal& actor, std::string_view operation, const Json& input,
    const WorkCheck& check)
{
    if (!tx.decision_enabled()) throw DecisionError(W::module_unavailable, "module_unavailable");
    if (operation == "decision_validate")
    {
        if (!actor.administrator) throw wire::Error(W::permission_denied);
        check_fields(input, {"sql"});
        Catalog catalog(tx, owner, check);
        const auto statement = parse(wire::string_field(input, "sql"));
        if (!decision_ddl(catalog, statement, false)) invalid();
        return {{"valid", true}};
    }
    if (operation == "decision_test")
    {
        check_fields(input, {"target", "rule", "from_version", "to_version", "cases", "revision"});
        return Inspection(engine, tx, owner, security, actor, input, check).compare(input);
    }
    if (operation != "decision_explain" && operation != "decision_whynot" && operation != "decision_whatif" &&
        operation != "decision_recheck") throw wire::Error(W::unsupported);
    check_fields(input, {"target", "event", "row_id", "proposed", "revision"});
    return Inspection(engine, tx, owner, security, actor, input, check).run(input);
}
}
