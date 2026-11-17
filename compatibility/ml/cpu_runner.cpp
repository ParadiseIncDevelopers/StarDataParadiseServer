#include "../../ml/clustering.hpp"
#include "../../ml/linear_models.hpp"
#include "../../ml/pca.hpp"
#include "../../ml/tree_models.hpp"
#ifdef SDP_WITH_CPU_MLP
#include "../../ml/mlp.hpp"
#endif

#include <exception>
#include <iostream>

namespace
{
using sdp::wire::Json;

Json fit(const Json& request, const std::string& algorithm)
{
    const auto features = request.at("features").get<std::vector<std::vector<double>>>();
    const auto& options = request.at("options");
    const auto seed = request.at("seed").get<uint32_t>();
    if (algorithm == "kmeans" || algorithm == "dbscan")
        return sdp::ml::fit_clustering(algorithm, features, options, seed);
    if (algorithm == "pca")
        return sdp::ml::fit_pca(features, options);

    const auto labels = request.at("labels").get<std::vector<double>>();
#ifdef SDP_WITH_CPU_MLP
    if (algorithm == "mlp")
    {
        const auto validation = request.at("validation").get<std::vector<std::vector<double>>>();
        const auto validation_labels = request.at("validation_labels").get<std::vector<double>>();
        return sdp::ml::fit_mlp(features, labels, validation, validation_labels, options, seed);
    }
#endif
    if (algorithm == "tree" || algorithm == "forest")
        return sdp::ml::fit_tree_model(algorithm, features, labels, options, seed);
    return sdp::ml::fit_linear_model(algorithm, features, labels, options);
}

Json predict(const Json& request, const std::string& algorithm)
{
    const auto features = request.at("features").get<std::vector<double>>();
    const auto& model = request.at("model");
    if (algorithm == "kmeans")
        return sdp::ml::assign_cluster(model, features);
    if (algorithm == "pca")
        return sdp::ml::transform_pca(model, features);
#ifdef SDP_WITH_CPU_MLP
    if (algorithm == "mlp")
        return sdp::ml::predict_mlp(model, features);
#endif
    if (algorithm == "tree" || algorithm == "forest")
        return sdp::ml::predict_tree_model(algorithm, model, features);
    return sdp::ml::predict_linear_model(algorithm, model, features);
}
}

int main()
{
    try
    {
        const auto request = Json::parse(std::cin);
        const auto action = request.at("action").get<std::string>();
        const auto algorithm = request.at("algorithm").get<std::string>();
        if (action == "train")
            std::cout << fit(request, algorithm).dump() << '\n';
        else if (action == "predict")
            std::cout << predict(request, algorithm).dump() << '\n';
        else
        {
            std::cerr << "Unknown CPU model operation.\n";
            return 1;
        }
    }
    catch (const std::exception&)
    {
        std::cerr << "The original CPU model engine rejected the request.\n";
        return 1;
    }
}
