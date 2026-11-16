#include "scheduled_retrain.hpp"

#include "worker.hpp"
#include "../ml/model.hpp"
#include "../ml/training.hpp"
#include "../sql/wire_values.hpp"

#include <limits>

namespace sdp::jobs
{
namespace
{
using wire::Json;
using W = wire::Code;
constexpr Budget training_budget{20'000, 128, 65'536};

struct Binding
{
    sql::ObjectId model;
    std::string algorithm;
    ml::Dataset dataset;
};

Binding bind(disk::Transaction& tx, disk::Owner owner, const sql::Security& security,
    const sql::Principal& actor, const Json& recipe, bool mlp_enabled, const std::function<void()>& check)
{
    const auto& text = wire::string_field(recipe, "name");
    if (text.empty() || text.size() > 392) throw wire::Error(W::invalid_argument);
    sql::Catalog catalog(tx, owner, check);
    const auto model = ml::require_model(catalog, sql::parse_history_target(text), actor);
    const auto algorithm = model.metadata.at("algorithm").get<std::string>();
    if (algorithm == "mlp" && !mlp_enabled) throw wire::Error(W::unsupported);
    auto dataset = ml::require_dataset(tx, owner, wire::string_field(recipe, "dataset_id"), actor);
    ml::require_dataset_read(dataset, catalog, security, actor);
    return {model.object.id, algorithm, std::move(dataset)};
}

Json training_payload(const Binding& binding, const Json& recipe)
{
    const auto& prepared = binding.dataset.content.at("prepared");
    const auto& features = prepared.at("features");
    const auto& target = prepared.at("target");
    if (ml::supervised_algorithm(binding.algorithm))
    {
        if (!target.is_array() || target.size() != features.size()) throw wire::Error(W::invalid_argument);
        const auto classes = prepared.at("target_classes").size();
        if (classes && (binding.algorithm == "linear" || classes != 2)) throw wire::Error(W::invalid_argument);
    }
    Json payload{{"kind", ml::training_kind(binding.algorithm)}, {"algorithm", binding.algorithm},
        {"seed", recipe.at("seed")}, {"options", recipe.at("options")}, {"features", features},
        {"target", target}, {"split", prepared.at("split")}};
    if (!valid_worker_request(payload, training_budget)) throw wire::Error(W::invalid_argument);
    return payload;
}

Binding bind_pinned(disk::Transaction& tx, disk::Owner owner, const sql::Security& security,
    const sql::Principal& actor, const Json& config, bool mlp_enabled, const std::function<void()>& check)
{
    if (config.at("actor_id") != std::to_string(actor.id)) throw wire::Error(W::permission_denied);
    auto binding = bind(tx, owner, security, actor, config.at("payload"), mlp_enabled, check);
    if (config.at("model") != binding.model || config.at("algorithm") != binding.algorithm ||
        config.at("dataset") != binding.dataset.id ||
        config.at("dataset_revision") != binding.dataset.content.at("revision"))
        throw wire::Error(W::invalid_state);
    return binding;
}
}

Json pin_retrain(disk::Transaction& tx, disk::Owner owner, const sql::Security& security,
    const sql::Principal& actor, const Json& request, bool mlp_enabled, const std::function<void()>& check)
{
    const auto seed = sql::unsigned_field(request, "seed", (std::numeric_limits<uint32_t>::max)());
    Json options = request.contains("options") ? wire::object_field(request, "options") : Json::object();
    if (options.dump().size() > 1024) throw wire::Error(W::resource_limit);
    auto binding = bind(tx, owner, security, actor, request, mlp_enabled, check);
    Json recipe{{"kind", "retrain"}, {"name", wire::string_field(request, "name")},
        {"dataset_id", binding.dataset.id}, {"seed", seed}, {"options", std::move(options)}};
    (void)training_payload(binding, recipe);
    return {{"payload", std::move(recipe)}, {"model", binding.model}, {"algorithm", binding.algorithm},
        {"actor_id", std::to_string(actor.id)}, {"dataset", binding.dataset.id},
        {"dataset_revision", binding.dataset.content.at("revision")}, {"timeout_ms", training_budget.timeout_ms},
        {"memory_mib", training_budget.memory_mib}, {"output_bytes", training_budget.output_bytes}};
}

Json prepare_retrain(disk::Transaction& tx, disk::Owner owner, const sql::Security& security,
    const sql::Principal& actor, const Json& config, bool mlp_enabled, const std::function<void()>& check)
{
    return training_payload(bind_pinned(tx, owner, security, actor, config, mlp_enabled, check), config.at("payload"));
}

void authorize_retrain(disk::Transaction& tx, disk::Owner owner, const sql::Security& security,
    const sql::Principal& actor, const Json& config, bool mlp_enabled, const std::function<void()>& check)
{
    (void)bind_pinned(tx, owner, security, actor, config, mlp_enabled, check);
}
}
