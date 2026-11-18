#include "cpu_compute.hpp"

#include "../../ml/clustering.hpp"
#include "../../ml/linear_models.hpp"
#include "../../ml/pca.hpp"
#include "../../ml/tree_models.hpp"
#ifdef SDP_WITH_CPU_MLP
#include "../../ml/mlp.hpp"
#endif

#include <chrono>
#include <cmath>
#include <limits>

namespace sdp::mysql_model
{
namespace
{
using wire::Json;
using Row = std::vector<double>;

struct TrainingRows
{
    std::vector<Row> features;
    Row labels;
    std::vector<Row> validation;
    Row validation_labels;
};

bool is_unsupervised(std::string_view algorithm)
{
    return algorithm == "kmeans" || algorithm == "dbscan" || algorithm == "pca";
}

Json fit(std::string_view algorithm, const TrainingRows& rows, const Json& options, uint32_t seed,
    const sql::WorkCheck& check = {})
{
    if (algorithm == "kmeans" || algorithm == "dbscan")
        return ml::fit_clustering(algorithm, rows.features, options, seed, check);
    if (algorithm == "pca")
        return ml::fit_pca(rows.features, options, check);
#ifdef SDP_WITH_CPU_MLP
    if (algorithm == "mlp")
        return ml::fit_mlp(rows.features, rows.labels, rows.validation, rows.validation_labels, options, seed, check);
#endif
    if (algorithm == "tree" || algorithm == "forest")
        return ml::fit_tree_model(algorithm, rows.features, rows.labels, options, seed, check);
    return ml::fit_linear_model(algorithm, rows.features, rows.labels, options, check);
}

Json predict(std::string_view algorithm, const Json& model, const Row& features)
{
    if (algorithm == "kmeans")
        return ml::assign_cluster(model, features);
    if (algorithm == "pca")
        return ml::transform_pca(model, features);
#ifdef SDP_WITH_CPU_MLP
    if (algorithm == "mlp")
        return ml::predict_mlp(model, features);
#endif
    if (algorithm == "tree" || algorithm == "forest")
        return ml::predict_tree_model(algorithm, model, features);
    return ml::predict_linear_model(algorithm, model, features);
}

[[noreturn]] void invalid()
{
    throw wire::Error(wire::Code::invalid_argument);
}

uint64_t unsigned_value(const Json& value, uint64_t maximum)
{
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0))
        invalid();
    const auto number = value.get<uint64_t>();
    if (number > maximum) invalid();
    return number;
}

double finite_number(const Json& value)
{
    if (!value.is_number()) invalid();
    const auto number = value.get<double>();
    if (!std::isfinite(number)) invalid();
    return number;
}

Row sql_features(const Json& input)
{
    if (!input.is_array() || input.empty() || input.size() > 64) invalid();
    Row values;
    values.reserve(input.size());
    for (const auto& value : input) values.push_back(finite_number(value));
    return values;
}

TrainingRows sql_training_rows(const Json& input, std::string_view algorithm)
{
    if (!input.is_array() || input.empty() || input.size() > 1024) invalid();

    // JSON_ARRAYAGG makes no guarantee about result order. The SELECT supplies
    // each row's rank and the number of keys equal under the SQL collation.
    std::vector<const Json*> ordered(input.size());
    for (const auto& row : input)
    {
        if (!row.is_array() || row.size() != 6) invalid();
        const auto ordinal = unsigned_value(row[0], input.size());
        if (ordinal == 0 || ordered[ordinal - 1] || unsigned_value(row[1], 1) != 1 ||
            unsigned_value(row[2], 0) != 0)
            invalid();
        ordered[ordinal - 1] = &row;
    }

    TrainingRows training;
    const bool supervised = !is_unsupervised(algorithm);
    const bool mlp = algorithm == "mlp";
    for (const auto* row : ordered)
    {
        auto features = sql_features((*row)[3]);
        const auto& label = (*row)[4];
        const auto& partition = (*row)[5];
        if (mlp && partition == "validation")
        {
            training.validation.push_back(std::move(features));
            training.validation_labels.push_back(finite_number(label));
        }
        else
        {
            if (mlp ? partition != "train" : !partition.is_null()) invalid();
            training.features.push_back(std::move(features));
            if (supervised) training.labels.push_back(finite_number(label));
            else if (!label.is_null()) invalid();
        }
    }
    return training;
}
}

wire::Json compute(const Json& request)
{
    const auto& action = request.at("action").get_ref<const std::string&>();
    const auto& algorithm = request.at("algorithm").get_ref<const std::string&>();
    if (action == "predict")
        return predict(algorithm, request.at("model"), request.at("features").get<Row>());
    if (action != "train") invalid();

    TrainingRows rows;
    rows.features = request.at("features").get<std::vector<Row>>();
    if (!is_unsupervised(algorithm)) rows.labels = request.at("labels").get<Row>();
#ifdef SDP_WITH_CPU_MLP
    if (algorithm == "mlp")
    {
        rows.validation = request.at("validation").get<std::vector<Row>>();
        rows.validation_labels = request.at("validation_labels").get<Row>();
    }
#endif
    return fit(algorithm, rows, request.at("options"), request.at("seed").get<uint32_t>());
}

wire::Json compute_sql(const Json& request)
{
    if (!request.is_object()) invalid();
    const auto& action = request.at("action").get_ref<const std::string&>();
    const auto& algorithm = request.at("algorithm").get_ref<const std::string&>();
    if (action == "predict" && request.size() == 4)
        return predict(algorithm, request.at("model"), sql_features(request.at("features")));
    if (action != "train" || request.size() != 5) invalid();

    const auto& options = request.at("options");
    if (!options.is_object() || options.dump().size() > 1024) invalid();
    const auto seed = static_cast<uint32_t>(unsigned_value(request.at("seed"), (std::numeric_limits<uint32_t>::max)()));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    return fit(algorithm, sql_training_rows(request.at("rows"), algorithm), options, seed, [deadline] {
        if (std::chrono::steady_clock::now() >= deadline)
            throw wire::Error(wire::Code::timeout);
    });
}
}
