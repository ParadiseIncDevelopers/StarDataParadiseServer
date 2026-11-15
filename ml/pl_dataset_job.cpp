#include "pl_dataset_job.hpp"
#include "../sql/pl_runtime.hpp"
#include "../sql/wire_values.hpp"

#include <algorithm>

namespace sdp::jobs
{
namespace
{
using wire::Json;
constexpr size_t source_limit = 8192;
constexpr size_t call_limit = 64;
constexpr pl::Limits routine_limits{8192, 12, 512 * 1024, std::chrono::milliseconds(500)};

[[noreturn]] void invalid(const char* message)
{
    throw sql::Error(sql::Code::unsupported, {}, message);
}

bool valid_calls(const Json& calls)
{
    return calls.is_array() && !calls.empty() && calls.size() <= call_limit &&
        std::ranges::all_of(calls, [](const Json& call) { return call.is_object() && call.size() <= 64; });
}

struct Routine
{
    pl::Program program;
    sql::ParameterTypes inputs;
    Json columns = Json::array();
    bool is_record{};
};

Routine compile_routine(std::string_view source)
{
    if (source.empty() || source.size() > source_limit) invalid("Dataset routine source bound exceeded");
    const auto statement = sql::parse(source);
    if (statement.action != sql::Action::create_routine || !statement.routine)
        invalid("Dataset job requires a stored PL/SQL function or procedure");
    Routine result;
    result.program = pl::compile(*statement.routine);
    const auto& program = result.program;
    if (!program.deterministic || program.uses_sql || program.writes_sql ||
        !program.cursors.empty() || !program.out_cursors.empty())
        invalid("Dataset routine must run without SQL or host access");
    const auto column = [&](const std::string& name, sql::Type type) {
        result.columns.push_back({{"name", name}, {"type", sql::type_name(type)}});
        if (type.kind == sql::Kind::record) result.is_record = true;
    };
    if (program.returns) column("return_value", *program.returns);
    for (const auto& slot : std::span(program.slots).first(program.parameters))
    {
        if (slot.mode != pl::Mode::out) result.inputs.emplace(slot.name, slot.type);
        if (slot.mode != pl::Mode::in)
        {
            if (program.returns && slot.name == "return_value")
                invalid("Dataset return_value column name is reserved");
            column(slot.name, slot.type);
        }
    }
    if (result.columns.empty()) invalid("Dataset routine has no output");
    if (result.columns.size() > 1) result.is_record = true;
    return result;
}

sql::RowValues read_arguments(const Routine& routine, const Json& call)
{
    const auto values = sql::read_parameters(call, routine.inputs);
    sql::RowValues bindings;
    bindings.reserve(values.size());
    for (const auto& slot : std::span(routine.program.slots).first(routine.program.parameters))
        if (slot.mode != pl::Mode::out) bindings.push_back(values.at(slot.name));
    return bindings;
}
}

uint64_t dataset_routine_calls(const Json& payload)
{
    if (!payload.is_object() || payload.size() != 3 || !payload.contains("kind") ||
        payload["kind"] != "dataset_plsql" || !payload.contains("source") ||
        !payload["source"].is_string() || !payload.contains("calls") || !valid_calls(payload["calls"]))
        return 0;
    const auto& source = payload["source"].get_ref<const std::string&>();
    if (source.empty() || source.size() > source_limit) return 0;
    return payload["calls"].size();
}

void validate_dataset_routine(std::string_view source)
{
    (void)compile_routine(source);
}

void validate_dataset_calls(std::string_view source, const Json& calls)
{
    if (!valid_calls(calls)) invalid("Dataset calls require 1 to 64 argument objects");
    const auto routine = compile_routine(source);
    for (const auto& call : calls) (void)read_arguments(routine, call);
}

std::optional<Json> run_dataset_routine(const Json& payload, size_t output_limit,
    const std::function<void(uint64_t done)>& progress)
{
    const auto total = dataset_routine_calls(payload);
    if (!total) invalid("Dataset worker payload is invalid");
    const auto routine = compile_routine(payload.at("source").get_ref<const std::string&>());
    Json output{{"kind", total > 1 ? "result_set" : routine.is_record ? "record" : "scalar"},
        {"columns", routine.columns}, {"rows", Json::array()}};
    size_t bytes = output.dump().size();
    if (bytes > output_limit) return {};
    for (const auto& call : payload.at("calls"))
    {
        const auto args = read_arguments(routine, call);
        const auto result = pl::invoke(routine.program, args, {}, {}, routine_limits);
        Json row = Json::array();
        if (routine.program.returns) row.push_back(sql::write_value(*result.returned, *routine.program.returns));
        for (const auto& value : result.out) row.push_back(sql::write_value(value.value, value.type));

        auto& rows = output.at("rows");
        const auto added = row.dump().size() + static_cast<size_t>(!rows.empty());
        if (added > output_limit - bytes) return {};
        bytes += added;
        rows.push_back(std::move(row));
        if (progress) progress(rows.size());
    }
    return output;
}
}
