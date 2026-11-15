#include "linear_models.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace sdp::ml
{
namespace
{
using wire::Json;
using W = wire::Code;
using Matrix = std::vector<std::vector<double>>;
using Rows = std::span<const std::vector<double>>;

constexpr size_t max_width = 64;
constexpr size_t max_rows = 1024;
constexpr size_t logistic_budget = 100'000'000;
constexpr double value_limit = 1e100;

[[noreturn]] void invalid() { throw wire::Error(W::invalid_argument); }
[[noreturn]] void limit() { throw wire::Error(W::resource_limit); }

void check_now(const sql::WorkCheck& check)
{
    if (check) check();
}

void shape(const Json& value, std::initializer_list<std::string_view> fields)
{
    if (!value.is_object()) invalid();
    for (const auto& item : value.items())
        if (std::ranges::find(fields, item.key()) == fields.end()) invalid();
}

const Json& field(const Json& object, const char* key)
{
    const auto found = object.find(key);
    if (found == object.end()) invalid();
    return *found;
}

double as_number(const Json& value)
{
    if (!value.is_number()) invalid();
    const auto parsed = value.get<double>();
    if (!std::isfinite(parsed)) invalid();
    return parsed;
}

double real_option(const Json& options, const char* key, double fallback, double minimum, double maximum)
{
    if (!options.contains(key)) return fallback;
    const auto result = as_number(options.at(key));
    if (result < minimum || result > maximum) invalid();
    return result;
}

size_t iterations_option(const Json& options)
{
    if (!options.contains("iterations")) return 512;
    const auto& input = options.at("iterations");
    if (!input.is_number_integer() || (input.is_number_unsigned() && input.get<uint64_t>() > 2000)) invalid();
    const auto parsed = input.get<int64_t>();
    if (parsed < 1 || parsed > 2000) invalid();
    return static_cast<size_t>(parsed);
}

bool valid_value(double value) { return std::isfinite(value) && std::abs(value) <= value_limit; }

bool binary(std::string_view algorithm)
{
    if (algorithm == "linear") return false;
    if (algorithm == "logistic") return true;
    invalid();
}

void valid_input(Rows x, std::span<const double> y, bool classification, const sql::WorkCheck& check)
{
    if (x.empty() || x.size() != y.size() || x.front().empty()) invalid();
    const auto width = x.front().size();
    if (x.size() > max_rows || width > max_width) limit();
    for (const auto& row : x)
    {
        check_now(check);
        if (row.size() != width || !std::ranges::all_of(row, valid_value)) invalid();
    }
    if (!std::ranges::all_of(y, valid_value)) invalid();
    if (classification)
    {
        if (!std::ranges::all_of(y, [](double label) { return label == 0 || label == 1; })) invalid();
        if (std::ranges::find(y, 0.0) == y.end() || std::ranges::find(y, 1.0) == y.end()) invalid();
    }
}

struct Normalization
{
    std::vector<double> center;
    std::vector<double> scale;
};

Normalization fit_normalizer(Rows x, const sql::WorkCheck& check)
{
    Normalization fit;
    const auto width = x.front().size();
    fit.center.reserve(width);
    fit.scale.reserve(width);
    for (size_t column = 0; column < width; ++column)
    {
        check_now(check);
        const auto anchor = x.front()[column];
        double offset{};
        for (const auto& row : x) offset += row[column] - anchor;
        const auto center = anchor + offset / static_cast<double>(x.size());
        double norm{};
        for (const auto& row : x) norm = std::hypot(norm, row[column] - center);
        const auto deviation = norm / std::sqrt(static_cast<double>(x.size()));
        fit.center.push_back(center);
        fit.scale.push_back(deviation > 0 ? deviation : 1.0);
    }
    return fit;
}

Matrix normalize(Rows x, const Normalization& fit, const sql::WorkCheck& check)
{
    Matrix result;
    result.reserve(x.size());
    for (const auto& row : x)
    {
        check_now(check);
        auto& normalized = result.emplace_back();
        normalized.reserve(row.size());
        for (size_t i = 0; i < row.size(); ++i)
            normalized.push_back((row[i] - fit.center[i]) / fit.scale[i]);
    }
    return result;
}

struct Model
{
    double intercept{};
    std::vector<double> coefficients;
};

double sq_norm(const Matrix& matrix, size_t offset, size_t column)
{
    double result{};
    for (size_t i = offset; i < matrix.size(); ++i) result += matrix[i][column] * matrix[i][column];
    return result;
}

Model least_squares(Rows x, std::span<const double> y, double ridge, const sql::WorkCheck& check)
{
    const auto width = x.front().size() + 1;
    Matrix design;
    design.reserve(x.size() + width - 1);
    for (const auto& row : x)
    {
        auto& point = design.emplace_back(1, 1.0);
        point.insert(point.end(), row.begin(), row.end());
    }
    auto targets = std::vector<double>(y.begin(), y.end());
    if (ridge > 0)
    {
        const auto penalty = std::sqrt(ridge * static_cast<double>(x.size()));
        for (size_t column = 1; column < width; ++column)
        {
            design.emplace_back(width, 0.0)[column] = penalty;
            targets.push_back(0.0);
        }
    }

    std::vector<size_t> permutation(width);
    std::iota(permutation.begin(), permutation.end(), size_t{0});
    const auto tolerance = std::numeric_limits<double>::epsilon() * 16 * static_cast<double>((std::max)(design.size(), width));
    double tolerance_sq{};
    size_t rank{};
    const auto maximum_rank = (std::min)(width, design.size());
    for (size_t k = 0; k < maximum_rank; ++k)
    {
        check_now(check);
        size_t pivot = k;
        double pivot_sq{};
        for (size_t column = k; column < width; ++column)
        {
            const auto norm = sq_norm(design, k, column);
            if (norm <= pivot_sq) continue;
            pivot_sq = norm;
            pivot = column;
        }
        if (!k) tolerance_sq = pivot_sq * tolerance * tolerance;
        if (pivot_sq <= tolerance_sq) break;
        if (pivot != k)
        {
            for (auto& point : design) std::swap(point[k], point[pivot]);
            std::swap(permutation[k], permutation[pivot]);
        }

        // Reflect the selected column onto its diagonal, avoiding normal equations.
        const auto diagonal = -std::copysign(std::sqrt(pivot_sq), design[k][k]);
        auto reflector = std::vector<double>(design.size() - k);
        for (size_t i = k; i < design.size(); ++i) reflector[i - k] = design[i][k];
        reflector.front() -= diagonal;
        const auto denominator = std::inner_product(reflector.begin(), reflector.end(), reflector.begin(), 0.0);
        const auto factor = 2.0 / denominator;
        for (size_t column = k + 1; column < width; ++column)
        {
            double projection{};
            for (size_t i = k; i < design.size(); ++i) projection += reflector[i - k] * design[i][column];
            projection *= factor;
            for (size_t i = k; i < design.size(); ++i) design[i][column] -= projection * reflector[i - k];
        }
        const auto target_projection = factor *
            std::inner_product(reflector.begin(), reflector.end(), targets.begin() + static_cast<ptrdiff_t>(k), 0.0);
        for (size_t i = k; i < design.size(); ++i) targets[i] -= target_projection * reflector[i - k];
        design[k][k] = diagonal;
        for (size_t i = k + 1; i < design.size(); ++i) design[i][k] = 0.0;
        ++rank;
    }

    std::vector<double> solution(width, 0.0);
    for (size_t remaining = rank; remaining > 0; --remaining)
    {
        const auto row = remaining - 1;
        double rhs = targets[row];
        for (size_t column = row + 1; column < rank; ++column)
            rhs -= design[row][column] * solution[column];
        solution[row] = rhs / design[row][row];
    }
    std::vector<double> ordered(width);
    for (size_t k = 0; k < width; ++k) ordered[permutation[k]] = solution[k];
    return {ordered.front(), {ordered.begin() + 1, ordered.end()}};
}

double sigmoid(double value)
{
    if (value >= 0) return 1.0 / (1.0 + std::exp(-value));
    const auto scale = std::exp(value);
    return scale / (1.0 + scale);
}

double dot(double bias, std::span<const double> coefficients, std::span<const double> row)
{
    for (size_t i = 0; i < row.size(); ++i) bias = std::fma(coefficients[i], row[i], bias);
    return bias;
}

struct Evaluation
{
    double loss{};
    Model gradient;
};

Evaluation evaluate(Rows x, std::span<const double> y, const Model& point, double l2, bool gradient, size_t& budget,
    const sql::WorkCheck& check)
{
    const auto batch_cost = x.size() * (point.coefficients.size() + 1);
    if (batch_cost > budget) limit();
    budget -= batch_cost;
    Evaluation result;
    if (gradient) result.gradient.coefficients.resize(point.coefficients.size());
    for (size_t i = 0; i < x.size(); ++i)
    {
        if (i % 64 == 0) check_now(check);
        const auto logit = dot(point.intercept, point.coefficients, x[i]);
        const auto signed_error = y[i] == 0 ? logit : -logit;
        result.loss += (std::max)(signed_error, 0.0) + std::log1p(std::exp(-std::abs(signed_error)));
        if (!gradient) continue;
        const auto residual = sigmoid(logit) - y[i];
        result.gradient.intercept += residual;
        for (size_t i_col = 0; i_col < point.coefficients.size(); ++i_col)
            result.gradient.coefficients[i_col] += residual * x[i][i_col];
    }
    const auto n = static_cast<double>(x.size());
    result.loss /= n;
    result.gradient.intercept /= n;
    for (size_t i = 0; i < point.coefficients.size(); ++i)
    {
        const auto value = point.coefficients[i];
        result.loss += 0.5 * l2 * value * value;
        if (gradient) result.gradient.coefficients[i] = result.gradient.coefficients[i] / n + l2 * value;
    }
    return result;
}

Model logistic_fit(Rows x, std::span<const double> y, double l2, double step, size_t iterations,
    const sql::WorkCheck& check)
{
    const auto positives = std::count(y.begin(), y.end(), 1.0);
    const auto negatives = static_cast<ptrdiff_t>(y.size()) - positives;
    Model point{std::log(static_cast<double>(positives) / static_cast<double>(negatives)),
        std::vector<double>(x.front().size())};
    size_t budget = logistic_budget;
    for (size_t i = 0; i < iterations; ++i)
    {
        check_now(check);
        const auto measured = evaluate(x, y, point, l2, true, budget, check);
        const auto& g = measured.gradient;
        const auto gradient_sq = std::inner_product(g.coefficients.begin(), g.coefficients.end(),
            g.coefficients.begin(), g.intercept * g.intercept);
        if (gradient_sq < 1e-16) break;

        bool accepted = false;
        for (int backtrack = 0; backtrack < 24; ++backtrack)
        {
            auto candidate = point;
            candidate.intercept -= step * g.intercept;
            for (size_t column = 0; column < point.coefficients.size(); ++column)
                candidate.coefficients[column] -= step * g.coefficients[column];
            const auto loss = evaluate(x, y, candidate, l2, false, budget, check).loss;
            if (loss <= measured.loss - 1e-4 * step * gradient_sq)
            {
                point = std::move(candidate);
                accepted = true;
                break;
            }
            step *= 0.5;
        }
        if (!accepted) break;
    }
    return point;
}

Json artifact_for(const Model& model, const Normalization& fit)
{
    double intercept = model.intercept;
    std::vector<double> coefficients;
    coefficients.reserve(model.coefficients.size());
    for (size_t i = 0; i < model.coefficients.size(); ++i)
    {
        const auto value = model.coefficients[i] / fit.scale[i];
        if (!std::isfinite(value)) limit();
        coefficients.push_back(value);
        intercept = std::fma(-value, fit.center[i], intercept);
    }
    if (!std::isfinite(intercept)) limit();
    return {{"intercept", intercept}, {"coefficients", std::move(coefficients)},
        {"normalized", {{"center", fit.center}, {"scale", fit.scale},
            {"intercept", model.intercept}, {"coefficients", model.coefficients}}}};
}

std::vector<double> numbers(const Json& values, size_t width)
{
    if (!values.is_array() || values.size() != width) invalid();
    std::vector<double> parsed;
    parsed.reserve(width);
    for (const auto& value : values) parsed.push_back(as_number(value));
    return parsed;
}
}

Json fit_linear_model(std::string_view algorithm, Rows x, std::span<const double> y, const Json& options,
    const sql::WorkCheck& check)
{
    const auto classification = binary(algorithm);
    valid_input(x, y, classification, check);
    if (classification) shape(options, {"iterations", "learning_rate", "l2"});
    else shape(options, {"ridge"});
    const auto normalizer = fit_normalizer(x, check);
    const auto inputs = normalize(x, normalizer, check);
    const auto fitted = classification ? logistic_fit(inputs, y,
        real_option(options, "l2", 1e-4, 0, 1000),
        real_option(options, "learning_rate", 1, 1e-6, 10), iterations_option(options), check)
        : least_squares(inputs, y, real_option(options, "ridge", 0, 0, 1000), check);
    return artifact_for(fitted, normalizer);
}

double predict_linear_model(std::string_view algorithm, const Json& artifact, std::span<const double> row)
{
    const auto classification = binary(algorithm);
    shape(artifact, {"intercept", "coefficients", "normalized"});
    if (row.empty() || row.size() > max_width || !std::ranges::all_of(row, valid_value)) invalid();
    auto coefficients = numbers(field(artifact, "coefficients"), row.size());
    auto intercept = as_number(field(artifact, "intercept"));
    auto normalized = std::vector<double>();
    if (artifact.contains("normalized"))
    {
        const auto& config = artifact.at("normalized");
        shape(config, {"center", "scale", "intercept", "coefficients"});
        const auto centers = numbers(field(config, "center"), row.size());
        const auto scales = numbers(field(config, "scale"), row.size());
        normalized.reserve(row.size());
        for (size_t i = 0; i < row.size(); ++i)
        {
            if (scales[i] <= 0) invalid();
            const auto input = (row[i] - centers[i]) / scales[i];
            if (!std::isfinite(input)) invalid();
            normalized.push_back(input);
        }
        row = normalized;
        coefficients = numbers(field(config, "coefficients"), row.size());
        intercept = as_number(field(config, "intercept"));
    }
    const auto result = dot(intercept, coefficients, row);
    if (!std::isfinite(result)) invalid();
    return classification ? sigmoid(result) : result;
}
}
