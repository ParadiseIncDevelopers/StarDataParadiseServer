#include "tree_models.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <optional>
#include <random>

namespace sdp::ml
{
namespace
{
using wire::Json;
using Row = std::vector<double>;
constexpr size_t max_rows = 4096;
constexpr size_t max_columns = 128;
constexpr size_t max_depth = 6;
constexpr size_t max_trees = 16;
constexpr size_t max_tree_nodes = 127;
constexpr size_t max_total_nodes = 512;
constexpr size_t split_work_limit = 15'000'000;

[[noreturn]] void invalid(const char* message)
{
    throw sql::Error(sql::Code::type, {}, message);
}

void poll(const sql::WorkCheck& check)
{
    if (check) check();
}

bool forest(std::string_view algorithm)
{
    if (algorithm == "tree") return false;
    if (algorithm == "forest") return true;
    invalid("Unknown tree model algorithm");
}

size_t bounded_integer(const Json& value, size_t lower, size_t upper, const char* message)
{
    if (!value.is_number_integer()) invalid(message);
    if (!value.is_number_unsigned() && value.get<int64_t>() < 0) invalid(message);
    const auto number = value.get<uint64_t>();
    if (number < lower || number > upper) invalid(message);
    return static_cast<size_t>(number);
}

double finite(const Json& value, const char* message)
{
    if (!value.is_number()) invalid(message);
    const auto number = value.get<double>();
    if (!std::isfinite(number)) invalid(message);
    return number;
}

struct Options
{
    size_t depth;
    size_t min_leaf;
    size_t trees;
    size_t features;
};

Options read_options(const Json& options, bool ensemble, size_t width)
{
    if (!options.is_object()) invalid("Tree model options must be an object");
    for (const auto& item : options.items())
        if (item.key() != "max_depth" && item.key() != "min_leaf" &&
            !(ensemble && (item.key() == "trees" || item.key() == "max_features")))
            invalid("Unknown tree model option");
    const auto option = [&](const char* key, size_t bound, size_t fallback) {
        return options.contains(key) ? bounded_integer(options.at(key), 1, bound, "Tree option out of range") : fallback;
    };
    const auto sampled = static_cast<size_t>(std::sqrt(static_cast<double>(width)));
    return {option("max_depth", max_depth, 4), option("min_leaf", 64, 1),
        option("trees", max_trees, ensemble ? 8 : 1), option("max_features", width, ensemble ? sampled : width)};
}

size_t draw(std::mt19937_64& rng, size_t exclusive)
{
    const auto bound = static_cast<uint64_t>(exclusive);
    const auto reject_below = (uint64_t{} - bound) % bound;
    uint64_t value{};
    do { value = rng(); } while (value < reject_below);
    return static_cast<size_t>(value % bound);
}

std::vector<size_t> features(std::mt19937_64& rng, size_t width, size_t sampled)
{
    std::vector<size_t> result(width);
    std::iota(result.begin(), result.end(), size_t{});
    if (sampled == width) return result;
    for (size_t index = 0; index < sampled; ++index)
        std::swap(result[index], result[index + draw(rng, width - index)]);
    result.resize(sampled);
    std::ranges::sort(result);
    return result;
}

double impurity(size_t positives, size_t total)
{
    return static_cast<double>(positives) * static_cast<double>(total - positives) / static_cast<double>(total);
}

struct Split
{
    size_t feature;
    double threshold;
};

struct Sample
{
    double value;
    bool positive;
};

class Builder
{
public:
    Builder(std::span<const Row> x, std::span<const double> y, Options options, uint64_t seed,
        const sql::WorkCheck& check)
        : x_(x), y_(y), width_(x.front().size()), options_(options), rng_(seed), check_(check)
    {}

    Json build_tree(size_t limit, bool bootstrap)
    {
        std::vector<size_t> rows(x_.size());
        if (bootstrap) for (auto& row : rows) row = draw(rng_, x_.size());
        else std::iota(rows.begin(), rows.end(), size_t{});
        auto tree = Json::array({Json()});
        grow(tree, 0, rows, 0, limit);
        return tree;
    }

private:
    std::optional<Split> choose(std::span<const size_t> rows, size_t positives)
    {
        std::optional<Split> best;
        double score = impurity(positives, rows.size());
        std::vector<Sample> ordered;
        ordered.reserve(rows.size());
        constexpr auto epsilon = 1e-12;
        for (const auto feature : features(rng_, width_, options_.features))
        {
            poll(check_);
            if (rows.size() > split_work_limit - work_)
                throw sql::Error(sql::Code::limit, {}, "Tree split work budget reached");
            work_ += rows.size();
            ordered.clear();
            for (auto row : rows) ordered.push_back({x_[row][feature], y_[row] == 1.0});
            std::ranges::sort(ordered, {}, &Sample::value);
            size_t left_positives{};
            for (size_t left = 1; left < ordered.size(); ++left)
            {
                left_positives += static_cast<size_t>(ordered[left - 1].positive);
                const auto right = ordered.size() - left;
                if (left < options_.min_leaf || right < options_.min_leaf ||
                    ordered[left - 1].value == ordered[left].value)
                    continue;
                const auto candidate = impurity(left_positives, left) + impurity(positives - left_positives, right);
                if (score - candidate <= epsilon) continue;
                score = candidate;
                double threshold = std::midpoint(ordered[left - 1].value, ordered[left].value);
                if (threshold == ordered[left].value) threshold = ordered[left - 1].value;
                best = Split{feature, threshold};
            }
        }
        return best;
    }

    void grow(Json& tree, size_t index, std::span<const size_t> rows, size_t depth, size_t limit)
    {
        poll(check_);
        const auto positives = static_cast<size_t>(std::ranges::count_if(rows, [&](auto row) { return y_[row] == 1.0; }));
        const auto probability = static_cast<double>(positives) / static_cast<double>(rows.size());
        tree[index] = Json::array({probability});
        if (depth >= options_.depth || !positives || positives == rows.size() ||
            rows.size() < options_.min_leaf * 2 || tree.size() + 2 > limit)
            return;
        const auto split = choose(rows, positives);
        if (!split) return;
        std::vector<size_t> left, right;
        for (const auto row : rows)
            (x_[row][split->feature] <= split->threshold ? left : right).push_back(row);
        if (left.size() < options_.min_leaf || right.size() < options_.min_leaf)
            invalid("Tree split failed to satisfy its leaf bounds");
        const auto left_index = tree.size();
        tree.push_back(nullptr);
        const auto right_index = tree.size();
        tree.push_back(nullptr);
        tree[index] = Json::array({probability, split->feature, split->threshold, left_index, right_index});
        grow(tree, left_index, left, depth + 1, limit);
        grow(tree, right_index, right, depth + 1, limit);
    }

    std::span<const Row> x_;
    std::span<const double> y_;
    size_t width_;
    Options options_;
    std::mt19937_64 rng_;
    const sql::WorkCheck& check_;
    size_t work_{};
};

struct Node
{
    double probability{};
    size_t feature{};
    double threshold{};
    size_t left{}, right{};
};

double predict_tree(const Json& tree, std::span<const double> row)
{
    if (!tree.is_array() || tree.empty() || tree.size() > max_tree_nodes) invalid("Invalid tree node array");
    std::array<Node, max_tree_nodes> nodes{};
    std::array<size_t, max_tree_nodes> depth{};
    depth[0] = 1;
    for (size_t index = 0; index < tree.size(); ++index)
    {
        const auto& wire_node = tree[index];
        if (!depth[index] || depth[index] > max_depth + 1 || !wire_node.is_array() ||
            (wire_node.size() != 1 && wire_node.size() != 5))
            invalid("Invalid tree branch");
        auto& node = nodes[index];
        node.probability = finite(wire_node[0], "Invalid tree probability");
        if (node.probability < 0 || node.probability > 1) invalid("Invalid tree probability");
        if (wire_node.size() == 1) continue;
        node.feature = bounded_integer(wire_node[1], 0, row.size() - 1, "Invalid tree split feature");
        node.threshold = finite(wire_node[2], "Invalid tree split threshold");
        node.left = bounded_integer(wire_node[3], index + 1, tree.size() - 1, "Invalid tree left link");
        node.right = bounded_integer(wire_node[4], index + 1, tree.size() - 1, "Invalid tree right link");
        if (node.left == node.right || depth[node.left] || depth[node.right])
            invalid("Tree node has more than one parent");
        depth[node.left] = depth[node.right] = depth[index] + 1;
    }
    size_t index{};
    while (nodes[index].left)
    {
        const auto& node = nodes[index];
        index = row[node.feature] <= node.threshold ? node.left : node.right;
    }
    return nodes[index].probability;
}
}

Json fit_tree_model(std::string_view algorithm, std::span<const Row> x, std::span<const double> y,
    const Json& options, uint64_t seed, const sql::WorkCheck& check)
{
    const bool ensemble = forest(algorithm);
    if (x.empty() || x.size() > max_rows || x.size() != y.size() || x.front().empty() || x.front().size() > max_columns)
        invalid("Invalid tree training shape");
    const auto width = x.front().size();
    for (size_t index = 0; index < x.size(); ++index)
    {
        poll(check);
        if ((y[index] != 0.0 && y[index] != 1.0) || x[index].size() != width ||
            !std::ranges::all_of(x[index], [](double value) { return std::isfinite(value); }))
            invalid("Tree training requires finite inputs and binary labels");
    }
    const auto settings = read_options(options, ensemble, width);
    Builder builder(x, y, settings, seed, check);
    auto trees = Json::array();
    size_t available = max_total_nodes;
    for (size_t index = 0; index < settings.trees; ++index)
    {
        const auto limit = (std::min)(max_tree_nodes, available / (settings.trees - index));
        auto tree = builder.build_tree(limit, ensemble);
        available -= tree.size();
        trees.push_back(std::move(tree));
    }
    return {{"format", 1}, {"width", width}, {"trees", std::move(trees)}};
}

double predict_tree_model(std::string_view algorithm, const Json& artifact, std::span<const double> row)
{
    const bool ensemble = forest(algorithm);
    if (!artifact.is_object() || artifact.size() != 3 || !artifact.contains("format") ||
        !artifact["format"].is_number_integer() || artifact["format"] != 1 ||
        !artifact.contains("width") || !artifact.contains("trees"))
        invalid("Unsupported tree model artifact");
    const auto width = bounded_integer(artifact["width"], 1, max_columns, "Invalid tree artifact width");
    const auto& trees = artifact["trees"];
    if (row.size() != width || !std::ranges::all_of(row, [](double value) { return std::isfinite(value); }) ||
        !trees.is_array() || trees.empty() || trees.size() > (ensemble ? max_trees : 1))
        invalid("Tree prediction requires a matching finite feature row");
    size_t nodes{};
    double sum{};
    for (const auto& tree : trees)
    {
        if (!tree.is_array() || tree.size() > max_total_nodes - nodes) invalid("Tree artifact node budget exceeded");
        nodes += tree.size();
        sum += predict_tree(tree, row);
    }
    return sum / static_cast<double>(trees.size());
}
}
