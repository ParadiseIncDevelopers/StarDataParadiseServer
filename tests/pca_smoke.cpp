#include "pca.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>

namespace
{
using sdp::ml::fit_pca;
using sdp::ml::reconstruct_pca;
using sdp::ml::transform_pca;
using sdp::wire::Code;
using sdp::wire::Json;
using Matrix = std::vector<std::vector<double>>;

void require(bool accepted, const char* reason)
{
    if (!accepted) throw std::runtime_error(reason);
}

void near(double actual, double expected, double tolerance, const char* reason)
{
    require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, reason);
}

template<class Function>
void expect(Code code, const Function& function)
{
    try { function(); }
    catch (const sdp::wire::Error& error)
    {
        require(error.code() == code, "unexpected PCA error status");
        return;
    }
    throw std::runtime_error("expected PCA error");
}

void test_analytic_variance()
{
    const Matrix train{{-3.0, 0.0}, {3.0, 0.0}, {0.0, -1.0}, {0.0, 1.0}};
    const auto model = fit_pca(train, {{"components", 1}});
    require(model.at("components") == Json::array({Json::array({1.0, 0.0})}), "top diagonal axis differs");
    near(model.at("explained_variance").at(0).get<double>(), 6.0, 1e-12, "sample covariance is incorrect");
    near(model.at("total_variance").get<double>(), 20.0 / 3.0, 1e-12, "discarded variance vanished");
    near(model.at("explained_variance_ratio").at(0).get<double>(), 0.9, 1e-12, "one-axis variance ratio differs");
    const auto score = transform_pca(model, std::array{2.0, -3.0});
    require(score == std::vector<double>{2.0} &&
        reconstruct_pca(model, score) == std::vector<double>{2.0, 0.0}, "1D projection or lossy reconstruction differs");
}

void test_nondiagonal_covariance()
{
    const Matrix train{{-2.0, -2.0}, {-1.0, -1.0}, {1.0, 1.0}, {2.0, 2.0}};
    const auto fitted = fit_pca(train, {{"components", 2}});
    require(fitted == fit_pca(train, {{"components", 2}}), "tied directions were nondeterministic");
    const auto model = Json::parse(fitted.dump());
    const auto first = model.at("components").at(0).get<std::vector<double>>();
    const auto second = model.at("components").at(1).get<std::vector<double>>();
    for (auto element : first)
        near(element, 1.0 / std::sqrt(2.0), 1e-12, "Jacobi eigenvector did not follow the rotated axis");
    near(std::inner_product(first.begin(), first.end(), second.begin(), 0.0), 0, 1e-12,
        "Jacobi components are not orthogonal");
    near(model.at("explained_variance").at(0).get<double>(), 20.0 / 3.0, 1e-12, "rotated variance differs");
    near(model.at("explained_variance").at(1).get<double>(), 0.0, 1e-12, "rank-deficient variance is nonzero");
    const auto before = model.dump();
    const std::array held_out{8.0, -3.0};
    const auto rebuilt = reconstruct_pca(model, transform_pca(model, held_out));
    for (size_t i = 0; i < rebuilt.size(); ++i)
        near(rebuilt[i], held_out[i], 1e-12, "full-dimensional PCA round trip lost a held-out feature");
    require(model.dump() == before, "hold-out transform or reconstruction re-fitted the PCA state");
}

void test_scaling_and_constant()
{
    const Matrix huge{{1e15}, {1e15 + 0.125}};
    const auto precise = fit_pca(huge, {{"components", 1}});
    near(precise.at("explained_variance").at(0).get<double>(), 0.0078125, 1e-12,
        "a large feature offset biased the variance");
    const auto transformed = transform_pca(precise, huge.front());
    near(transformed.front(), -0.0625, 1e-12, "rounded displayed center changed scores");
    require(reconstruct_pca(precise, transformed) == huge.front(), "large-offset full reconstruction differs");

    const Matrix tiny{{-1e-200}, {1e-200}};
    const auto scaled = fit_pca(tiny, {{"components", 1}});
    require(scaled.at("variance_reason").is_null() && scaled.at("explained_variance_ratio").at(0) == 1.0,
        "variance ratio was lost to absolute covariance underflow");

    const Matrix flat{{7, 3}, {7, 3}, {7, 3}};
    const auto constant = fit_pca(flat, {{"components", 2}});
    require(constant.at("variance_reason") == "zero_total_variance" &&
        constant.at("explained_variance_ratio") == Json::array({nullptr, nullptr}),
        "undefined constant-data variance ratio was fabricated");
    require(transform_pca(constant, flat.front()) == std::vector<double>(2, 0.0) &&
        reconstruct_pca(constant, std::array{0.0, 0.0}) == flat.front(), "constant data failed reconstruction");
}

void test_invalid_and_cancel()
{
    const Matrix train{{3, 2, 0}, {1, 3, 6}, {4, -1, 2}, {-2, 7, 8}};
    const auto wanted = Json{{"components", 3}};
    expect(Code::invalid_argument, [&] { (void)fit_pca(Matrix{{1.0}}, {{"components", 1}}); });
    expect(Code::invalid_argument, [&] { (void)fit_pca(train, {{"components", 0}}); });
    expect(Code::invalid_argument, [&] { (void)fit_pca(train, {{"components", 4}}); });
    expect(Code::invalid_argument, [&] { (void)fit_pca(Matrix{{1}, {2, 3}}, {{"components", 1}}); });
    expect(Code::invalid_argument, [&] {
        const Matrix invalid_input{{1}, {(std::numeric_limits<double>::infinity)()}};
        (void)fit_pca(invalid_input, {{"components", 1}});
    });
    expect(Code::resource_limit, [&] {
        (void)fit_pca(Matrix(2, std::vector<double>(65, 1)), {{"components", 1}});
    });
    size_t calls{};
    expect(Code::cancelled, [&] {
        (void)fit_pca(train, wanted, [&] {
            if (++calls == 2 * train.size() + 3) throw sdp::wire::Error(Code::cancelled);
        });
    });
    auto model = fit_pca(train, wanted);
    expect(Code::invalid_argument, [&] { (void)transform_pca(model, std::array{1.0, 2.0}); });
    expect(Code::invalid_argument, [&] { (void)reconstruct_pca(model, std::array{1.0, 2.0}); });
    model["components"][0] = std::vector<double>(3, 0.0);
    expect(Code::invalid_argument, [&] { (void)transform_pca(model, train.front()); });
}
}

int main()
{
    try
    {
        test_analytic_variance();
        test_nondiagonal_covariance();
        test_scaling_and_constant();
        test_invalid_and_cancel();
        std::cout << "PCA variance reference, Jacobi rotations, hold-out reconstruction, constant and scale bounds passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
