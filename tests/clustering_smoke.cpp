#include "../ml/clustering.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>

namespace
{
using namespace sdp;
using wire::Json;
using Matrix = std::vector<std::vector<double>>;

void require(bool passed, const char* message)
{
    if (!passed) throw std::runtime_error(message);
}

template<class Operation>
void expect(Operation operation, sql::Code code, const char* reason)
{
    try { operation(); }
    catch (const sql::Error& error)
    {
        require(error.code() == code, reason);
        return;
    }
    throw std::runtime_error(reason);
}

void kmeans()
{
    const Matrix x{{0}, {2}, {10}, {12}};
    const Json options{{"k", 2}};
    const auto result = ml::fit_clustering("kmeans", x, options, 117);
    require(result == ml::fit_clustering("kmeans", x, options, 117), "K-Means seed did not reproduce the result");
    const auto& labels = result.at("labels");
    const auto& centers = result.at("centers");
    require(labels[0] == labels[1] && labels[2] == labels[3] && labels[0] != labels[2] &&
        centers[labels[0].get<size_t>()] == Json::array({1.0}) &&
        centers[labels[2].get<size_t>()] == Json::array({11.0}) && result.at("inertia") == 4.0 &&
        result.at("nonempty_clusters") == 2 && result.at("converged") == true,
        "K-Means means, labels or inertia differ from the known clusters");
    require(ml::assign_cluster(result, std::array{7.0}) == labels[2].get<size_t>() &&
        ml::assign_cluster(result, std::array{6.0}) == 0,
        "K-Means out-of-sample assignment or deterministic tie differed");
    expect([&] { (void)ml::fit_clustering("kmeans", Matrix{{5}, {5}}, options, 1); }, sql::Code::type,
        "K-Means accepted more centers than distinguishable feature rows");
    expect([&] { (void)ml::fit_clustering("kmeans", x, {{"k", 0}}, 1); }, sql::Code::type,
        "K-Means accepted k=0");

    auto damaged = result;
    damaged["centers"][0] = Json::array();
    expect([&] { (void)ml::assign_cluster(damaged, std::array{8.0}); }, sql::Code::type,
        "K-Means accepted an invalid persisted center");
    expect([&] { (void)ml::assign_cluster(result, std::array{std::numeric_limits<double>::quiet_NaN()}); },
        sql::Code::type, "K-Means accepted nonfinite prediction");
}

void dbscan()
{
    const Matrix x{{0.0}, {0.25}, {0.35}, {0.45}, {5}, {5.1}, {5.2}, {20}};
    const auto result = ml::fit_clustering("dbscan", x, {{"eps", 0.3}, {"min_points", 3}}, 1);
    require(result.at("labels") == Json::array({0, 0, 0, 0, 1, 1, 1, -1}) &&
        result.at("clusters") == 2 && result.at("noise") == 1,
        "DBSCAN did not attach an earlier visited border row or count one noise row");
    expect([&] { (void)ml::assign_cluster(result, std::array{0.3}); }, sql::Code::unsupported,
        "DBSCAN claimed out-of-sample prediction");
    expect([&] { (void)ml::fit_clustering("dbscan", x, {{"eps", 0}, {"min_points", 3}}, 1); },
        sql::Code::type, "DBSCAN accepted zero eps");
    expect([&] { (void)ml::fit_clustering("dbscan", x, {{"eps", 0.3}, {"min_points", 0}}, 1); },
        sql::Code::type, "DBSCAN accepted zero min_points");
}

void metric()
{
    const Matrix x{{0}, {2}, {10}, {12}, {50}};
    const std::array<int64_t, 5> label{0, 0, 1, 1, -1};
    const auto score = ml::silhouette(x, label);
    const auto reference = (9.0 / 11 + 7.0 / 9) / 2;
    require(std::abs(score.at("value").get<double>() - reference) < 1e-12 &&
        score.at("clusters") == 2 && score.at("samples") == 4 && score.at("reason").is_null(),
        "Silhouette disagrees with independent per-row averages excluding noise");
    const std::array<int64_t, 5> only{0, 0, 0, 0, -1};
    const auto undefined = ml::silhouette(x, only);
    require(undefined.at("value").is_null() && undefined.at("reason") == "insufficient_clusters",
        "Single-cluster silhouette reported a numeric score");
    require(ml::silhouette({}, {}).at("reason") == "insufficient_samples", "Empty silhouette lacked a reason");
    const auto singleton = ml::silhouette(Matrix{{0}, {2}, {10}}, std::array<int64_t, 3>{0, 0, 1});
    require(std::abs(singleton.at("value").get<double>() - ((0.8 + 0.75) / 3)) < 1e-12,
        "Singleton contribution differed from the documented policy");
}

void quota()
{
    const Matrix x{{0}, {100}};
    bool cancelled{};
    try { (void)ml::fit_clustering("kmeans", x, {{"k", 2}}, 1,
        [] { throw wire::Error(wire::Code::cancelled); }); }
    catch (const wire::Error& error) { cancelled = error.code() == wire::Code::cancelled; }
    require(cancelled, "Clustering ignored cancellation");
    Matrix large(1536, std::vector<double>(64, 0.0));
    for (size_t index = 0; index < large.size(); ++index) large[index][0] = static_cast<double>(index);
    expect([&] { (void)ml::fit_clustering("dbscan", large, {{"eps", 0.1}, {"min_points", 2}}, 1); },
        sql::Code::limit, "DBSCAN did not enforce its actual distance work budget");
}
}

int main()
{
    try
    {
        kmeans();
        dbscan();
        metric();
        quota();
        std::cout << "Seeded K-Means, held-out assignment, DBSCAN border/noise, exact silhouette and quotas passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
