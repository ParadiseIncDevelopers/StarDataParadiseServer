#include "clustering.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <map>
#include <numeric>
#include <random>

namespace sdp::ml
{
namespace
{
using wire::Json;
using Row = std::vector<double>;
using Data = std::span<const Row>;
constexpr size_t max_rows = 2048;
constexpr size_t max_columns = 64;
constexpr size_t max_clusters = 16;
constexpr size_t max_distance_work = 64'000'000;
constexpr auto coordinate_limit = 1e100;
constexpr auto radius_limit = 1e102;
constexpr int64_t unvisited = -2, noise = -1;

[[noreturn]] void invalid(const char* message)
{
    throw sql::Error(sql::Code::type, {}, message);
}

void poll(const sql::WorkCheck& check)
{
    if (check) check();
}

bool coordinate(double value)
{
    return std::isfinite(value) && std::abs(value) <= coordinate_limit;
}

size_t validate(Data x, const sql::WorkCheck& check)
{
    if (x.empty() || x.size() > max_rows || x.front().empty() || x.front().size() > max_columns)
        invalid("Clustering needs a non-empty bounded feature matrix");
    const auto width = x.front().size();
    for (const auto& row : x)
    {
        poll(check);
        if (row.size() != width || !std::ranges::all_of(row, coordinate))
            invalid("Clustering requires matching finite feature rows");
    }
    return width;
}

void option_names(const Json& options, std::initializer_list<std::string_view> allowed)
{
    if (!options.is_object()) invalid("Clustering options must be an object");
    for (const auto& field : options.items())
        if (std::ranges::find(allowed, field.key()) == allowed.end()) invalid("Unknown clustering option");
}

size_t integer(const Json& json, size_t first, size_t last, const char* message)
{
    if (!json.is_number_integer() || (!json.is_number_unsigned() && json.get<int64_t>() < 0))
        invalid(message);
    const auto value = json.get<uint64_t>();
    if (value < first || value > last) invalid(message);
    return static_cast<size_t>(value);
}

double number(const Json& json, double first, double last, const char* message)
{
    if (!json.is_number()) invalid(message);
    const auto value = json.get<double>();
    if (!std::isfinite(value) || value < first || value > last) invalid(message);
    return value;
}

size_t draw(std::mt19937_64& rng, size_t exclusive)
{
    const auto bound = static_cast<uint64_t>(exclusive);
    const auto reject_below = (uint64_t{} - bound) % bound;
    uint64_t value{};
    do { value = rng(); } while (value < reject_below);
    return static_cast<size_t>(value % bound);
}

double squared(std::span<const double> left, std::span<const double> right)
{
    double sum{};
    for (size_t feature = 0; feature < left.size(); ++feature)
    {
        const auto difference = left[feature] - right[feature];
        sum += difference * difference;
    }
    return sum;
}

class Distances
{
public:
    explicit Distances(const sql::WorkCheck& check) : check_(check) {}

    double operator()(std::span<const double> left, std::span<const double> right)
    {
        if (left.size() > max_distance_work - work_)
            throw sql::Error(sql::Code::limit, {}, "Clustering distance work budget reached");
        if ((work_ >> 11) != ((work_ + left.size()) >> 11)) poll(check_);
        work_ += left.size();
        return squared(left, right);
    }

private:
    const sql::WorkCheck& check_;
    size_t work_{};
};

template<class Distance>
std::pair<size_t, double> nearest(Data centers, std::span<const double> row, Distance&& distance)
{
    size_t chosen{};
    auto best = distance(row, centers.front());
    for (size_t index = 1; index < centers.size(); ++index)
    {
        const auto candidate = distance(row, centers[index]);
        if (candidate < best)
        {
            chosen = index;
            best = candidate;
        }
    }
    return {chosen, best};
}

std::vector<Row> initialize(Data x, size_t k, uint64_t seed, Distances& distance)
{
    std::mt19937_64 random(seed);
    std::vector<Row> centers{x[draw(random, x.size())]};
    std::vector<double> weights(x.size(), (std::numeric_limits<double>::max)());
    while (centers.size() < k)
    {
        double total{};
        for (size_t row = 0; row < x.size(); ++row)
        {
            weights[row] = (std::min)(weights[row], distance(x[row], centers.back()));
            total += weights[row];
        }
        if (!(total > 0)) invalid("K-Means k exceeds the number of distinguishable feature rows");
        const auto target = static_cast<double>(random() >> 11) * 0x1p-53 * total;
        double prefix{};
        size_t selected{};
        for (size_t row = 0; row < x.size(); ++row)
        {
            if (weights[row] == 0) continue;
            selected = row;
            prefix += weights[row];
            if (prefix > target) break;
        }
        centers.push_back(x[selected]);
    }
    return centers;
}

Json kmeans(Data x, size_t width, const Json& options, uint64_t seed, const sql::WorkCheck& check)
{
    option_names(options, {"k", "max_iterations", "tolerance"});
    if (!options.contains("k")) invalid("K-Means requires k");
    const auto k = integer(options["k"], 1, (std::min)(max_clusters, x.size()), "Invalid K-Means k");
    const auto limit = options.contains("max_iterations") ?
        integer(options["max_iterations"], 1, 200, "Invalid K-Means iteration limit") : 100;
    const auto tolerance = options.contains("tolerance") ?
        number(options["tolerance"], 0, radius_limit, "Invalid K-Means tolerance") : 1e-8;
    Distances distance(check);
    auto centers = initialize(x, k, seed, distance);
    bool converged{};
    size_t iterations{};
    while (iterations < limit && !converged)
    {
        poll(check);
        std::vector<Row> sums(k, Row(width));
        std::vector<size_t> counts(k);
        for (const auto& row : x)
        {
            const auto index = nearest(centers, row, distance).first;
            ++counts[index];
            for (size_t feature = 0; feature < width; ++feature) sums[index][feature] += row[feature];
        }
        converged = true;
        for (size_t index = 0; index < k; ++index)
        {
            if (!counts[index]) continue;
            for (auto& sum : sums[index])
                sum = std::clamp(sum / static_cast<double>(counts[index]), -coordinate_limit, coordinate_limit);
            if (distance(centers[index], sums[index]) > tolerance * tolerance) converged = false;
            centers[index] = std::move(sums[index]);
        }
        ++iterations;
    }
    std::vector<size_t> labels;
    labels.reserve(x.size());
    std::vector<bool> occupied(k);
    double inertia{};
    for (const auto& row : x)
    {
        const auto [label, cost] = nearest(centers, row, distance);
        labels.push_back(label);
        occupied[label] = true;
        inertia += cost;
    }
    return {{"format", 1}, {"algorithm", "kmeans"}, {"width", width}, {"centers", std::move(centers)},
        {"labels", std::move(labels)}, {"iterations", iterations}, {"converged", converged}, {"inertia", inertia},
        {"nonempty_clusters", std::ranges::count(occupied, true)}, {"empty_policy", "retain_previous_center"}};
}

std::vector<size_t> neighbors(Data x, size_t row, double radius, Distances& distance)
{
    std::vector<size_t> found;
    for (size_t candidate = 0; candidate < x.size(); ++candidate)
        if (distance(x[row], x[candidate]) <= radius) found.push_back(candidate);
    return found;
}

Json dbscan(Data x, size_t width, const Json& options, const sql::WorkCheck& check)
{
    option_names(options, {"eps", "min_points"});
    if (!options.contains("eps") || !options.contains("min_points"))
        invalid("DBSCAN requires eps and min_points");
    const auto eps = number(options["eps"], 0, radius_limit, "Invalid DBSCAN eps");
    const auto minimum = integer(options["min_points"], 1, x.size(), "Invalid DBSCAN min_points");
    const auto radius = eps * eps;
    if (radius == 0) invalid("DBSCAN eps must have a positive representable square");
    Distances distance(check);
    std::vector<int64_t> labels(x.size(), unvisited);
    int64_t cluster{};
    for (size_t row = 0; row < x.size(); ++row)
    {
        poll(check);
        if (labels[row] != unvisited) continue;
        auto queue = neighbors(x, row, radius, distance);
        if (queue.size() < minimum)
        {
            labels[row] = noise;
            continue;
        }
        labels[row] = cluster;
        std::vector<bool> queued(x.size());
        for (const auto member : queue) queued[member] = true;
        for (size_t head = 0; head < queue.size(); ++head)
        {
            const auto point = queue[head];
            if (labels[point] == noise)
            {
                labels[point] = cluster;
                continue;
            }
            if (labels[point] != unvisited) continue;
            labels[point] = cluster;
            const auto frontier = neighbors(x, point, radius, distance);
            if (frontier.size() < minimum) continue;
            for (const auto member : frontier)
                if (!queued[member] && labels[member] <= noise)
                {
                    queue.push_back(member);
                    queued[member] = true;
                }
        }
        ++cluster;
    }
    const auto noise_points = std::ranges::count(labels, noise);
    return {{"format", 1}, {"algorithm", "dbscan"}, {"width", width}, {"eps", eps}, {"min_points", minimum},
        {"labels", std::move(labels)}, {"clusters", cluster}, {"noise", noise_points}};
}

std::vector<Row> read_centers(const Json& artifact, size_t width)
{
    if (!artifact.contains("centers") || !artifact["centers"].is_array() ||
        artifact["centers"].empty() || artifact["centers"].size() > max_clusters)
        invalid("K-Means artifact has no valid centers");
    std::vector<Row> centers;
    for (const auto& center : artifact["centers"])
    {
        if (!center.is_array() || center.size() != width) invalid("K-Means center width differs");
        auto& row = centers.emplace_back();
        for (const auto& value : center)
            row.push_back(number(value, -coordinate_limit, coordinate_limit, "Invalid K-Means center value"));
    }
    return centers;
}
}

Json fit_clustering(std::string_view algorithm, Data x, const Json& options, uint64_t seed, const sql::WorkCheck& check)
{
    const auto width = validate(x, check);
    if (algorithm == "kmeans") return kmeans(x, width, options, seed, check);
    if (algorithm == "dbscan") return dbscan(x, width, options, check);
    throw sql::Error(sql::Code::unsupported, {}, "Unknown clustering algorithm");
}

size_t assign_cluster(const Json& artifact, std::span<const double> row)
{
    if (!artifact.is_object() || !artifact.contains("format") || !artifact["format"].is_number_integer() ||
        artifact["format"] != 1 || !artifact.contains("algorithm"))
        invalid("Invalid clustering artifact");
    if (artifact["algorithm"] == "dbscan")
        throw sql::Error(sql::Code::unsupported, {}, "DBSCAN does not assign out-of-sample clusters");
    if (artifact["algorithm"] != "kmeans" || !artifact.contains("width"))
        invalid("Invalid K-Means artifact");
    const auto width = integer(artifact["width"], 1, max_columns, "Invalid clustering feature width");
    if (row.size() != width || !std::ranges::all_of(row, coordinate))
        invalid("Cluster prediction needs a matching finite feature row");
    const auto centers = read_centers(artifact, width);
    return nearest(centers, row, squared).first;
}

Json silhouette(Data x, std::span<const int64_t> labels, const sql::WorkCheck& check)
{
    if (x.size() != labels.size()) invalid("Silhouette feature rows and labels differ");
    if (!x.empty()) validate(x, check);
    std::vector<size_t> points;
    std::map<int64_t, size_t> groups;
    std::vector<size_t> counts;
    std::vector<size_t> membership;
    for (size_t index = 0; index < labels.size(); ++index)
    {
        if (labels[index] == noise) continue;
        if (labels[index] < 0) invalid("Silhouette labels must be nonnegative or DBSCAN noise");
        const auto [entry, added] = groups.try_emplace(labels[index], groups.size());
        if (added) counts.push_back(0);
        ++counts[entry->second];
        points.push_back(index);
        membership.push_back(entry->second);
    }
    Json result{{"value", nullptr}, {"reason", nullptr}, {"samples", points.size()}, {"clusters", counts.size()}};
    if (points.size() < 3 || counts.size() >= points.size())
    {
        result["reason"] = "insufficient_samples";
        return result;
    }
    if (counts.size() < 2)
    {
        result["reason"] = "insufficient_clusters";
        return result;
    }

    Distances distance(check);
    std::vector<double> sums(counts.size());
    double total{};
    for (size_t index = 0; index < points.size(); ++index)
    {
        poll(check);
        const auto own = membership[index];
        if (counts[own] == 1) continue;
        std::ranges::fill(sums, 0.0);
        for (size_t other = 0; other < points.size(); ++other)
            if (other != index)
                sums[membership[other]] += std::sqrt(distance(x[points[index]], x[points[other]]));
        const auto a = sums[own] / static_cast<double>(counts[own] - 1);
        double b = (std::numeric_limits<double>::max)();
        for (size_t group = 0; group < counts.size(); ++group)
            if (group != own) b = (std::min)(b, sums[group] / static_cast<double>(counts[group]));
        const auto scale = (std::max)(a, b);
        if (scale > 0) total += (b - a) / scale;
    }
    result["value"] = total / static_cast<double>(points.size());
    return result;
}
}
