#include "../ml/tree_models.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>

namespace
{
using namespace sdp;
using wire::Json;

void require(bool passed, const char* message)
{
    if (!passed) throw std::runtime_error(message);
}

template<class Operation>
void expect_invalid(Operation operation, const char* context)
{
    try { operation(); }
    catch (const sql::Error& error)
    {
        require(error.code() == sql::Code::type, context);
        return;
    }
    throw std::runtime_error(context);
}

void test_cart()
{
    const std::vector<std::vector<double>> rows{
        {-4, 0}, {-3, 0}, {-2, 0}, {-1, 0}, {0, 0}, {1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}, {6, 0}, {7, 0}};
    const std::vector<double> label{0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0};
    const auto model = ml::fit_tree_model("tree", rows, label, {{"max_depth", 2}}, 1);
    require(model.at("trees").size() == 1 && model.at("trees")[0].size() == 5, "CART did not learn two decision cuts");
    require(ml::predict_tree_model("tree", model, std::array{-1.5, 0.0}) == 0 &&
        ml::predict_tree_model("tree", model, std::array{2.25, 0.0}) == 1 &&
        ml::predict_tree_model("tree", model, std::array{6.5, 0.0}) == 0,
        "Held-out points disagree with known interval labels");

    const auto pruned = ml::fit_tree_model("tree", rows, label, {{"min_leaf", 7}}, 99);
    require(pruned.at("trees")[0].size() == 1 &&
        ml::predict_tree_model("tree", pruned, std::array{2.25, 0.0}) == 0.25,
        "Minimum leaf did not use empirical root probability");
}

void test_forest()
{
    const std::vector<std::vector<double>> rows{
        {-4, -4}, {-3, -3}, {-2, -2}, {-1, -1}, {1, 1}, {2, 2}, {3, 3}, {4, 4}};
    const std::vector<double> label{0, 0, 0, 0, 1, 1, 1, 1};
    const auto model = ml::fit_tree_model("tree", rows, label, Json::object(), 1);
    require(model.at("trees")[0][0][1] == 0 && model.at("trees")[0][0][2] == 0,
        "Equal-gain tree features did not select the lowest feature and midpoint");
    const Json options{{"trees", 16}, {"max_features", 1}};
    const auto ensemble = ml::fit_tree_model("forest", rows, label, options, 24);
    require(ensemble == ml::fit_tree_model("forest", rows, label, options, 24) &&
        ensemble != ml::fit_tree_model("forest", rows, label, options, 25),
        "Forest seed did not reproducibly affect bootstrap and features");
    const std::array negative{-2.5, -2.5}, positive{2.5, 2.5};
    const auto predicted = ml::predict_tree_model("forest", ensemble, positive);
    require(ml::predict_tree_model("forest", ensemble, negative) < 0.5 && predicted >= 0.5,
        "Forest did not classify held-out extremes");
    std::array<bool, 2> used{};
    double sum{};
    for (const auto& tree : ensemble.at("trees"))
    {
        if (tree[0].size() > 1) used[tree[0][1].get<size_t>()] = true;
        auto individual = ensemble;
        individual["trees"] = Json::array({tree});
        sum += ml::predict_tree_model("tree", individual, positive);
    }
    require(used[0] && used[1] && std::abs(sum / 16 - predicted) < 1e-12,
        "Forest did not sample features or average empirical tree probabilities");

    auto broken = ensemble;
    broken["trees"][0] = Json::array({Json::array({0.5, 0, 0.0, 0, 0})});
    expect_invalid([&] { (void)ml::predict_tree_model("forest", broken, positive); },
        "Predictor accepted cyclic tree artifact links");
    expect_invalid([&] { (void)ml::predict_tree_model("forest", ensemble, std::array{1.0}); },
        "Predictor accepted mismatching feature width");
    expect_invalid([&] { (void)ml::fit_tree_model("tree", rows, label, {{"trees", 2}}, 0); },
        "CART accepted an ensemble-only option");
    auto invalid_rows = rows;
    invalid_rows.front()[0] = std::numeric_limits<double>::quiet_NaN();
    expect_invalid([&] { (void)ml::fit_tree_model("tree", invalid_rows, label, Json::object(), 0); },
        "Tree accepted a nonfinite input");
    auto invalid_label = label;
    invalid_label.front() = 2;
    expect_invalid([&] { (void)ml::fit_tree_model("tree", rows, invalid_label, Json::object(), 0); },
        "Tree accepted a nonbinary target");

    bool cancelled{};
    try { (void)ml::fit_tree_model("forest", rows, label, options, 24,
        [] { throw wire::Error(wire::Code::cancelled); }); }
    catch (const wire::Error& error) { cancelled = error.code() == wire::Code::cancelled; }
    require(cancelled, "Training ignored the job cancellation check");
}

void test_budget()
{
    std::vector<std::vector<double>> rows;
    std::vector<double> label;
    for (size_t index = 0; index < 512; ++index)
    {
        rows.push_back({static_cast<double>(index % 71), static_cast<double>(index % 97),
            static_cast<double>(index % 29), static_cast<double>(index % 107)});
        label.push_back(static_cast<double>(index % 2));
    }
    const auto model = ml::fit_tree_model("forest", rows, label,
        {{"trees", 16}, {"max_depth", 6}, {"max_features", 4}}, 91);
    size_t nodes{};
    for (const auto& tree : model.at("trees")) nodes += tree.size();
    require(nodes > 16 && nodes <= 512 && model.dump().size() < 64 * 1024,
        "Forest artifact exceeded the worker transfer budget");
    const auto probability = ml::predict_tree_model("forest", model, rows.front());
    require(probability >= 0 && probability <= 1, "Bounded forest returned a nonprobability");
}
}

int main()
{
    try
    {
        test_cart();
        test_forest();
        test_budget();
        std::cout << "CART Gini cuts, held-out predictions, seeded forest, strict artifact and budgets passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
