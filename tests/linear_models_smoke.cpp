#include "linear_models.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>

namespace
{
using sdp::ml::fit_linear_model;
using sdp::ml::predict_linear_model;
using sdp::wire::Code;
using sdp::wire::Json;
using Matrix = std::vector<std::vector<double>>;

void require(bool passed, const char* reason)
{
    if (!passed) throw std::runtime_error(reason);
}

void close(double actual, double expected, double tolerance, const char* reason)
{
    require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, reason);
}

template<class Function>
void expect(Code code, const Function& function)
{
    try { function(); }
    catch (const sdp::wire::Error& error)
    {
        require(error.code() == code, "unexpected ML error code");
        return;
    }
    throw std::runtime_error("expected ML error");
}

void test_ordinary_least_squares()
{
    const Matrix x{{-2, -1}, {-2, 1}, {0, -1}, {0, 1}, {2, -1}, {2, 1}};
    std::vector<double> y;
    for (const auto& row : x) y.push_back(1.5 + 3 * row[0] - 2 * row[1]);
    const auto free = fit_linear_model("linear", x, y, Json::object());
    close(free.at("intercept").get<double>(), 1.5, 1e-12, "regression intercept differs from reference");
    close(free.at("coefficients").at(0).get<double>(), 3, 1e-12, "first regression coefficient differs");
    close(free.at("coefficients").at(1).get<double>(), -2, 1e-12, "second regression coefficient differs");
    close(predict_linear_model("linear", free, std::array{4.0, 0.25}), 13, 1e-12,
        "held-out regression prediction differs from analytic expected value");
    const auto ridge = fit_linear_model("linear", x, y, {{"ridge", 1}});
    close(ridge.at("intercept").get<double>(), 1.5, 1e-12, "ridge penalized the intercept");
    close(ridge.at("coefficients").at(0).get<double>(), 1.5, 1e-12, "ridge failed to shrink the first orthogonal slope");
    close(ridge.at("coefficients").at(1).get<double>(), -1, 1e-12, "ridge failed to shrink the second orthogonal slope");
}

void test_conditioning()
{
    Matrix x;
    std::vector<double> y;
    for (int i = -5; i <= 5; ++i)
    {
        const auto value = static_cast<double>(i);
        x.push_back({value, 2 * value, 99.0});
        y.push_back(5 + 3 * value);
    }
    const auto deficient = fit_linear_model("linear", x, y, Json::object());
    close(predict_linear_model("linear", deficient, std::array{7.0, 14.0, 99.0}), 26, 1e-11,
        "rank-deficient regression did not recover the identifiable prediction");
    close(deficient.at("coefficients").at(2).get<double>(), 0, 0, "constant feature produced a fake slope");
    require(fit_linear_model("linear", x, y, Json::object()) == deficient,
        "rank-deficient fit did not choose a deterministic solution");

    x = {{1e15}, {1e15 + 0.125}, {1e15 + 0.25}, {1e15 + 0.75}, {1e15 + 0.875}};
    y = {4, 4.25, 4.5, 5.5, 5.75};
    const auto normalized = fit_linear_model("linear", x, y, Json::object());
    const auto round_trip = Json::parse(normalized.dump());
    close(predict_linear_model("linear", round_trip, std::array{1e15 + 0.375}), 4.75, 1e-12,
        "serialized inference lost precision on a large-offset feature");
}

double posterior(double value)
{
    return 1.0 / (1.0 + std::exp(-(1.4 * value - 0.3)));
}

void test_logistic()
{
    Matrix x;
    std::vector<double> y;
    for (int value = -2; value <= 2; ++value)
    {
        const auto p = posterior(value);
        const auto count = static_cast<int>(std::lround(80.0 * p));
        for (int sample = 0; sample < 80; ++sample)
        {
            x.push_back({static_cast<double>(value)});
            y.push_back(sample < count ? 1 : 0);
        }
    }
    const auto model = fit_linear_model("logistic", x, y, {{"l2", 0}, {"learning_rate", 10}});
    for (const auto value : {-1.5, -0.5, 0.5, 1.5})
        close(predict_linear_model("logistic", model, std::array{value}), posterior(value), 0.018,
            "held-out logistic probability differs from known generator");
    const auto strong_penalty = fit_linear_model("logistic", x, y, {{"l2", 20}});
    require(std::abs(strong_penalty.at("coefficients").at(0).get<double>()) <
        std::abs(model.at("coefficients").at(0).get<double>()) / 4, "logistic L2 had no measurable effect");
    require(predict_linear_model("logistic", model, std::array{1e100}) == 1 &&
        predict_linear_model("logistic", model, std::array{-1e100}) == 0,
        "logistic sigmoid overflowed on a large finite logit");

    size_t calls{};
    expect(Code::cancelled, [&] {
        (void)fit_linear_model("logistic", x, y, Json::object(), [&] {
            if (++calls == 2 * x.size() + 15) throw sdp::wire::Error(Code::cancelled);
        });
    });
}

void test_rejections()
{
    const Matrix x{{1}, {2}};
    const std::array y{0.0, 1.0};
    expect(Code::invalid_argument, [&] { (void)fit_linear_model("logistic", x, std::array{1.0, 1.0}, Json::object()); });
    expect(Code::invalid_argument, [&] { (void)fit_linear_model("logistic", x, std::array{0.0, 2.0}, Json::object()); });
    expect(Code::invalid_argument, [&] { (void)fit_linear_model("logistic", x, y, {{"iterations", 2001}}); });
    expect(Code::invalid_argument, [&] { (void)fit_linear_model("logistic", x, y, {{"learning_rate", 0.0}}); });
    expect(Code::invalid_argument, [&] { (void)fit_linear_model("linear", x, y, {{"ridge", -1}}); });
    expect(Code::invalid_argument, [&] { (void)fit_linear_model("linear", Matrix{{1}, {2, 3}}, y, Json::object()); });
    expect(Code::invalid_argument, [&] {
        (void)fit_linear_model("linear", x, std::array{0.0, std::numeric_limits<double>::infinity()}, Json::object());
    });
    expect(Code::resource_limit, [&] {
        const Matrix excessive(2, std::vector<double>(65));
        (void)fit_linear_model("linear", excessive, y, Json::object());
    });
    auto artifact = fit_linear_model("linear", x, y, Json::object());
    expect(Code::invalid_argument, [&] { (void)predict_linear_model("linear", artifact, std::array{1.0, 2.0}); });
    artifact["normalized"]["scale"][0] = 0;
    expect(Code::invalid_argument, [&] { (void)predict_linear_model("linear", artifact, std::array{1.0}); });
}
}

int main()
{
    try
    {
        test_ordinary_least_squares();
        test_conditioning();
        test_logistic();
        test_rejections();
        std::cout << "Linear QR and ridge reference, conditioned reload, held-out logistic probabilities and finite budgets passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
