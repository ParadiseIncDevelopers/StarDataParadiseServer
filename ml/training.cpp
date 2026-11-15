#include "training.hpp"
#include "linear_models.hpp"
#include "tree_models.hpp"
#include "clustering.hpp"
#include "pca.hpp"
#if defined(SDP_WITH_CPU_MLP)
#include "mlp.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <set>
#include <thread>

namespace sdp::ml
{
namespace
{
using wire::Json;
using W = wire::Code;
using Matrix = std::vector<std::vector<double>>;
constexpr std::array<std::string_view, 3> splits{"train", "validation", "test"};

[[noreturn]] void invalid() { throw wire::Error(W::invalid_argument); }

double number(const Json& value)
{
    if (!value.is_number()) invalid();
    const auto result = value.get<double>();
    if (!std::isfinite(result) || std::abs(result) > 1e15) invalid();
    return result;
}

struct Input
{
    Matrix x;
    std::vector<double> y;
    std::array<std::vector<size_t>, 3> indices;
};

Input read_input(const Json& body, bool supervised, bool classifier)
{
    const auto& matrix = body.at("features");
    const auto& target = body.at("target");
    const auto& split = body.at("split");
    if (!matrix.is_array() || matrix.size() < 3 || matrix.size() > 1024 ||
        (supervised && (!target.is_array() || target.size() != matrix.size())) || !split.is_object()) invalid();
    Input input;
    size_t width{};
    for (size_t i = 0; i < matrix.size(); ++i)
    {
        const auto& row = matrix.at(i);
        if (!row.is_array() || row.empty() || row.size() > 64 || (width && row.size() != width)) invalid();
        width = row.size();
        auto& current = input.x.emplace_back();
        for (const auto& value : row) current.push_back(number(value));
        if (supervised)
        {
            const auto label = number(target.at(i));
            if (classifier && label != 0 && label != 1) invalid();
            input.y.push_back(label);
        }
    }
    std::set<size_t> consumed;
    for (size_t group = 0; group < splits.size(); ++group)
    {
        const auto& members = split.at(splits[group]);
        if (!members.is_array() || members.empty()) invalid();
        for (const auto& index : members)
        {
            if (!index.is_number_unsigned() || index.get<uint64_t>() >= input.x.size()) invalid();
            const auto id = index.get<size_t>();
            if (!consumed.insert(id).second) invalid();
            input.indices[group].push_back(id);
        }
    }
    if (consumed.size() != input.x.size()) invalid();
    return input;
}

Json evaluate(std::string_view algorithm, const Json& artifact, const Input& data, std::span<const size_t> indices)
{
    long double squares{}, absolute{}, cross_entropy{};
    uint64_t tp{}, tn{}, fp{}, fn{};
    for (const auto id : indices)
    {
        const auto predicted = predict_supervised(algorithm, artifact, data.x[id]);
        if (!std::isfinite(predicted)) invalid();
        if (algorithm == "mlp")
        {
            const auto probability = std::clamp(predicted, 1e-15, 1.0 - 1e-15);
            cross_entropy -= data.y[id] == 1.0 ? std::log(probability) : std::log1p(-probability);
        }
        if (algorithm == "linear")
        {
            const auto error = static_cast<long double>(predicted) - data.y[id];
            absolute += std::abs(error);
            squares += error * error;
        }
        else if (data.y[id] >= 0.5) predicted >= 0.5 ? ++tp : ++fn;
        else predicted >= 0.5 ? ++fp : ++tn;
    }
    if (algorithm == "linear")
    {
        const auto mse = static_cast<double>(squares / indices.size());
        return {{"count", indices.size()}, {"mae", static_cast<double>(absolute / indices.size())},
            {"mse", mse}, {"rmse", std::sqrt(mse)}};
    }
    auto reasons = Json::object();
    const auto metric = [&](const char* name, uint64_t successes, uint64_t total, const char* reason) -> Json {
        if (!total)
        {
            reasons[name] = reason;
            return nullptr;
        }
        return static_cast<double>(successes) / static_cast<double>(total);
    };
    const auto precision = metric("precision", tp, tp + fp, "no_positive_predictions");
    const auto recall = metric("recall", tp, tp + fn, "no_positive_labels");
    const auto f1 = metric("f1", tp * 2, tp * 2 + fp + fn, "no_positive_labels_or_predictions");
    Json report{{"count", indices.size()}, {"accuracy", static_cast<double>(tp + tn) / static_cast<double>(indices.size())},
        {"precision", precision}, {"recall", recall}, {"f1", f1}, {"confusion", {{"tp", tp}, {"tn", tn}, {"fp", fp}, {"fn", fn}}},
        {"undefined", std::move(reasons)}};
    if (algorithm == "mlp") report["loss"] = static_cast<double>(cross_entropy / indices.size());
    return report;
}

Json evaluate_unsupervised(std::string_view algorithm, const Json& artifact, const Input& data,
    std::span<const size_t> indices, size_t split)
{
    if (algorithm == "pca")
    {
        long double squares{};
        for (auto id : indices)
        {
            const auto& row = data.x[id];
            const auto scores = transform_pca(artifact, row);
            const auto restored = reconstruct_pca(artifact, scores);
            for (size_t i = 0; i < row.size(); ++i)
            {
                const auto delta = static_cast<long double>(restored[i]) - row[i];
                squares += delta * delta;
            }
        }
        const auto mse = static_cast<double>(squares / (indices.size() * data.x.front().size()));
        return {{"count", indices.size()}, {"mse", mse}, {"rmse", std::sqrt(mse)},
            {"variance", artifact.at("explained_variance_ratio")}};
    }
    if (algorithm == "dbscan" && split)
        return {{"count", indices.size()}, {"silhouette", nullptr}, {"silhouette_reason", "out_of_sample_unavailable"}};
    Matrix x;
    std::vector<int64_t> labels;
    for (auto id : indices)
    {
        x.push_back(data.x[id]);
        if (split) labels.push_back(static_cast<int64_t>(assign_cluster(artifact, data.x[id])));
    }
    if (!split) labels = artifact.at("labels").get<std::vector<int64_t>>();
    const auto score = silhouette(x, labels);
    return {{"count", indices.size()}, {"silhouette", score.at("value")},
        {"silhouette_reason", score.at("reason")}, {"silhouette_samples", score.at("samples")}};
}
}

bool mlp_compiled() noexcept
{
#if defined(SDP_WITH_CPU_MLP)
    return true;
#else
    return false;
#endif
}

std::string_view training_kind(std::string_view algorithm)
{
    if (algorithm == "mlp") return "mlp_training";
    return supervised_algorithm(algorithm) ? "supervised" : "unsupervised";
}

bool supervised_algorithm(std::string_view algorithm)
{
    return algorithm == "linear" || algorithm == "logistic" || algorithm == "tree" || algorithm == "forest" || algorithm == "mlp";
}

bool model_algorithm(std::string_view algorithm)
{
    return supervised_algorithm(algorithm) || algorithm == "kmeans" || algorithm == "dbscan" || algorithm == "pca";
}

uint64_t training_steps(const Json& payload)
{
    if (!payload.is_object() || payload.size() < 7 || payload.size() > 10 ||
        !payload.contains("kind") || !payload["kind"].is_string() ||
        !payload.contains("algorithm") || !payload["algorithm"].is_string() || !payload.contains("features") ||
        !payload.contains("target") || !payload.contains("split") || !payload.contains("options") ||
        !payload.contains("seed") || !payload["seed"].is_number_unsigned()) return 0;
    for (const auto& field : payload.items())
        {
            if (field.key() != "kind" && field.key() != "algorithm" && field.key() != "features" &&
                field.key() != "target" && field.key() != "split" && field.key() != "options" &&
                field.key() != "seed" && field.key() != "device" && field.key() != "pause_after_epoch" &&
                field.key() != "resume") return 0;
        }
    const auto& algorithm = payload["algorithm"].get_ref<const std::string&>();
    if ((payload.contains("pause_after_epoch") || payload.contains("resume")) && algorithm != "mlp") return 0;
    if (payload.contains("device") && !payload.at("device").is_object()) return 0;
    if (payload.contains("pause_after_epoch") && (!payload.at("pause_after_epoch").is_number_unsigned() ||
        payload.at("pause_after_epoch").get<uint64_t>() == 0)) return 0;
    if (payload.contains("resume") && !payload.at("resume").is_object()) return 0;
    if (!model_algorithm(algorithm) || (algorithm == "mlp" && !mlp_compiled()) || payload["kind"].get_ref<const std::string&>() != training_kind(algorithm))
        return 0;
    return 4;
}

double predict_supervised(std::string_view algorithm, const Json& artifact, std::span<const double> row)
{
    if (algorithm == "linear" || algorithm == "logistic") return predict_linear_model(algorithm, artifact, row);
    if (algorithm == "tree" || algorithm == "forest") return predict_tree_model(algorithm, artifact, row);
#if defined(SDP_WITH_CPU_MLP)
    if (algorithm == "mlp") return predict_mlp(artifact, row);
#endif
    invalid();
}

Json train_model(const Json& payload, const std::function<void(uint64_t)>& progress)
{
    if (!training_steps(payload)) invalid();
    const auto algorithm = payload.at("algorithm").get<std::string>();
    const bool supervised = supervised_algorithm(algorithm);
    const bool classifier = supervised && algorithm != "linear";
    const auto input = read_input(payload, supervised, classifier);
    Matrix x;
    std::vector<double> y;
    for (auto id : input.indices[0])
    {
        x.push_back(input.x[id]);
        if (supervised) y.push_back(input.y[id]);
    }
    if (classifier && (std::ranges::find(y, 0.0) == y.end() || std::ranges::find(y, 1.0) == y.end())) invalid();
    const auto seed = payload.at("seed").get<uint64_t>();
    const auto& options = payload.at("options");
    if (!options.is_object()) invalid();
    Json artifact;
    if (algorithm == "linear" || algorithm == "logistic") artifact = fit_linear_model(algorithm, x, y, options);
#if defined(SDP_WITH_CPU_MLP)
    else if (algorithm == "mlp")
    {
        Matrix validation;
        std::vector<double> labels;
        for (auto id : input.indices[1])
        {
            validation.push_back(input.x[id]);
            labels.push_back(input.y[id]);
        }
        const auto pause_epoch = payload.value("pause_after_epoch", uint32_t{});
        const auto resume = payload.value("resume", Json(nullptr));
        struct EpochPause { Json state; };
        MlpCheckpoint callback;
        if (pause_epoch)
        {
            callback = [pause_epoch](const Json& state) {
                const auto& settings = state.at("settings");
                if (state.at("iterator").at("epoch") == pause_epoch &&
                    pause_epoch < settings.at("epochs").get<uint32_t>() &&
                    state.at("stale_epochs").get<uint32_t>() < settings.at("patience").get<uint32_t>())
                    throw EpochPause{state};
            };
        }
        try
        {
            artifact = fit_mlp(x, y, validation, labels, options, seed, {}, resume, callback);
        }
        catch (EpochPause& pause)
        {
            if (progress) progress(4);
            return {{"format", 1}, {"algorithm", "mlp"}, {"runtime", mlp_runtime},
                {"checkpoint", std::move(pause.state)}};
        }
    }
#endif
    else if (supervised) artifact = fit_tree_model(algorithm, x, y, options, seed);
    else if (algorithm == "pca") artifact = fit_pca(x, options);
    else artifact = fit_clustering(algorithm, x, options, seed);
    if (progress) progress(1);
    auto metrics = Json::object();
    for (size_t i = 0; i < splits.size(); ++i)
    {
        metrics[splits[i]] = supervised ? evaluate(algorithm, artifact, input, input.indices[i]) :
            evaluate_unsupervised(algorithm, artifact, input, input.indices[i], i);
        if (progress) progress(i + 2);
    }
    Json hardware{{"backend", "cpu"}, {"logical_threads", std::thread::hardware_concurrency()}};
    if (payload.contains("device")) hardware.update(payload.at("device"));
    return {{"format", 1}, {"algorithm", algorithm}, {"width", input.x[0].size()}, {"seed", seed},
        {"artifact", std::move(artifact)}, {"metrics", std::move(metrics)}, {"hardware", std::move(hardware)}};
}

bool valid_training_result(const Json& output, std::string_view expected)
{
    return output.is_object() && output.value("format", 0) == 1 && output.value("algorithm", std::string{}) == expected &&
        output.contains("artifact") && output["artifact"].is_object() && output.contains("metrics") &&
        output["metrics"].is_object() && output["metrics"].size() == splits.size() &&
        output.contains("width") && output["width"].is_number_unsigned() && output["width"] > 0 && output["width"] <= 64 &&
        output.contains("seed") && output["seed"].is_number_unsigned() && output.contains("hardware") &&
        output["hardware"].is_object() && output["hardware"].value("backend", std::string{}) == "cpu";
}

bool valid_training_transfer(const Json& output, const Json& payload)
{
    const auto& algorithm = payload.at("algorithm").get_ref<const std::string&>();
    if (valid_training_result(output, algorithm)) return true;
    if (algorithm != "mlp" || !payload.contains("pause_after_epoch") || !output.is_object() ||
        output.size() != 4 || output.value("format", 0) != 1 || output.value("algorithm", std::string{}) != "mlp" ||
        output.value("runtime", std::string{}) != mlp_runtime || !output.contains("checkpoint")) return false;
    const auto& checkpoint = output.at("checkpoint");
    if (!checkpoint.is_object() || checkpoint.value("format", 0) != 1 || !checkpoint.contains("iterator") ||
        !checkpoint.at("iterator").is_object()) return false;
    const auto& cursor = checkpoint.at("iterator");
    return cursor.value("epoch", 0u) == payload.at("pause_after_epoch").get<uint32_t>() &&
        cursor.value("position", UINT32_MAX) == 0;
}

bool check_model_artifact(std::string_view algorithm, const Json& artifact, size_t width)
{
    if (!width || width > 64) return false;
    const std::vector<double> zero(width);
    if (supervised_algorithm(algorithm)) return std::isfinite(predict_supervised(algorithm, artifact, zero));
    if (algorithm == "pca") return reconstruct_pca(artifact, transform_pca(artifact, zero)).size() == width;
    if (algorithm == "kmeans") return assign_cluster(artifact, zero) < artifact.at("centers").size();
    if (algorithm == "dbscan")
    {
        if (artifact.value("format", 0) != 1 || artifact.value("width", size_t{}) != width) return false;
        const auto labels = artifact.at("labels").get<std::vector<int64_t>>();
        return !labels.empty() && labels.size() <= 1024 &&
            std::ranges::all_of(labels, [&](auto label) { return label >= -1 && label < static_cast<int64_t>(labels.size()); });
    }
    return false;
}
}
