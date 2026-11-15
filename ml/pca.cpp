#include "pca.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace sdp::ml
{
namespace
{
using wire::Json;
using W = wire::Code;
using Matrix = std::vector<std::vector<double>>;
using Rows = std::span<const std::vector<double>>;

constexpr size_t row_limit = 1024;
constexpr size_t width_limit = 64;
constexpr size_t sweep_limit = 64;
constexpr double value_limit = 1e100;

[[noreturn]] void invalid() { throw wire::Error(W::invalid_argument); }
[[noreturn]] void limit() { throw wire::Error(W::resource_limit); }

const Json& field(const Json& input, const char* key)
{
    const auto found = input.find(key);
    if (found == input.end()) invalid();
    return *found;
}

uint64_t nonnegative(const Json& input)
{
    if (input.is_number_unsigned()) return input.get<uint64_t>();
    if (!input.is_number_integer() || input.get<int64_t>() < 0) invalid();
    return static_cast<uint64_t>(input.get<int64_t>());
}

double number(const Json& input)
{
    if (!input.is_number()) invalid();
    const auto value = input.get<double>();
    if (!std::isfinite(value)) invalid();
    return value;
}

bool valid_feature(double value) { return std::isfinite(value) && std::abs(value) <= value_limit; }

void checkpoint(const sql::WorkCheck& check)
{
    if (check) check();
}

struct Centered
{
    std::vector<double> origin;
    std::vector<double> offset;
    Matrix rows;
    double scale{};
};

Centered center_rows(Rows rows, const sql::WorkCheck& check)
{
    const auto width = rows.front().size();
    Centered result{rows.front(), std::vector<double>(width), {}, 0.0};
    result.rows.reserve(rows.size());
    std::vector<double> correction(width);
    for (const auto& row : rows)
    {
        checkpoint(check);
        if (row.size() != width || !std::ranges::all_of(row, valid_feature)) invalid();
        auto& shifted = result.rows.emplace_back(width);
        for (size_t i = 0; i < width; ++i)
        {
            shifted[i] = row[i] - result.origin[i];
            const auto increment = shifted[i] - correction[i];
            const auto previous = result.offset[i];
            result.offset[i] += increment;
            correction[i] = (result.offset[i] - previous) - increment;
        }
    }
    for (auto& value : result.offset) value /= static_cast<double>(rows.size());
    for (auto& row : result.rows)
        for (size_t i = 0; i < width; ++i)
        {
            row[i] -= result.offset[i];
            result.scale = (std::max)(result.scale, std::abs(row[i]));
        }
    return result;
}

Matrix covariance(Centered& inputs, const sql::WorkCheck& check)
{
    const auto width = inputs.origin.size();
    Matrix symmetric(width, std::vector<double>(width));
    if (inputs.scale == 0) return symmetric;
    for (auto& row : inputs.rows)
    {
        checkpoint(check);
        for (auto& x : row) x /= inputs.scale;
        for (size_t i = 0; i < width; ++i)
            for (size_t j = i; j < width; ++j)
                symmetric[i][j] = std::fma(row[i], row[j], symmetric[i][j]);
    }
    const auto divisor = static_cast<double>(inputs.rows.size() - 1);
    for (size_t i = 0; i < width; ++i)
        for (size_t j = i; j < width; ++j)
        {
            symmetric[i][j] /= divisor;
            symmetric[j][i] = symmetric[i][j];
        }
    return symmetric;
}

void jacobi_rotate(Matrix& symmetric, Matrix& vectors, size_t left, size_t right)
{
    const auto off_diagonal = symmetric[left][right];
    const auto delta = (symmetric[right][right] - symmetric[left][left]) / (2.0 * off_diagonal);
    const auto tangent = std::copysign(1.0, delta) / (std::abs(delta) + std::hypot(1.0, delta));
    const auto cosine = 1.0 / std::hypot(1.0, tangent);
    const auto sine = tangent * cosine;
    symmetric[left][left] -= tangent * off_diagonal;
    symmetric[right][right] += tangent * off_diagonal;
    symmetric[left][right] = symmetric[right][left] = 0.0;

    for (size_t i = 0; i < symmetric.size(); ++i)
    {
        if (i != left && i != right)
        {
            const auto l = symmetric[i][left];
            const auto r = symmetric[i][right];
            symmetric[i][left] = symmetric[left][i] = cosine * l - sine * r;
            symmetric[i][right] = symmetric[right][i] = sine * l + cosine * r;
        }
        const auto l = vectors[i][left];
        const auto r = vectors[i][right];
        vectors[i][left] = cosine * l - sine * r;
        vectors[i][right] = sine * l + cosine * r;
    }
}

Matrix eigenvectors(Matrix& symmetric, const sql::WorkCheck& check)
{
    const auto width = symmetric.size();
    Matrix vectors(width, std::vector<double>(width));
    double largest{};
    for (size_t i = 0; i < width; ++i)
    {
        vectors[i][i] = 1.0;
        largest = (std::max)(largest, symmetric[i][i]);
    }
    if (largest == 0) return vectors;
    const auto tolerance = largest * (32 * std::numeric_limits<double>::epsilon()) * static_cast<double>(width);
    for (size_t sweep = 0; sweep < sweep_limit; ++sweep)
    {
        bool rotated{};
        for (size_t left = 0; left < width; ++left)
        {
            checkpoint(check);
            for (size_t right = left + 1; right < width; ++right)
            {
                if (std::abs(symmetric[left][right]) <= tolerance) continue;
                jacobi_rotate(symmetric, vectors, left, right);
                rotated = true;
            }
        }
        if (!rotated) return vectors;
    }
    for (size_t i = 0; i < width; ++i)
        for (size_t j = i + 1; j < width; ++j)
            if (std::abs(symmetric[i][j]) > tolerance) limit();
    return vectors;
}

std::vector<double> oriented_axis(const Matrix& vectors, size_t column)
{
    std::vector<double> axis;
    axis.reserve(vectors.size());
    for (const auto& row : vectors) axis.push_back(row[column]);
    const auto mark = std::ranges::max_element(axis, {}, [](double value) { return std::abs(value); });
    if (*mark < 0)
        for (auto& value : axis) value = -value;
    return axis;
}

std::vector<double> numbers(const Json& input, size_t size)
{
    if (!input.is_array() || input.size() != size) invalid();
    std::vector<double> values;
    values.reserve(size);
    for (const auto& item : input) values.push_back(number(item));
    return values;
}

struct Calibration
{
    std::vector<double> origin;
    std::vector<double> offset;
    Matrix axes;
};

Calibration read_artifact(const Json& artifact)
{
    if (!artifact.is_object() || nonnegative(field(artifact, "format")) != 1) invalid();
    const auto stored_width = nonnegative(field(artifact, "width"));
    if (!stored_width || stored_width > width_limit) invalid();
    const auto width = static_cast<size_t>(stored_width);
    Calibration decoded{numbers(field(artifact, "origin"), width), numbers(field(artifact, "mean_offset"), width), {}};
    const auto centers = numbers(field(artifact, "center"), width);
    for (size_t i = 0; i < width; ++i)
        if (!valid_feature(decoded.origin[i]) || centers[i] != decoded.origin[i] + decoded.offset[i]) invalid();

    const auto& axes = field(artifact, "components");
    if (!axes.is_array() || axes.empty() || axes.size() > width) invalid();
    decoded.axes.reserve(axes.size());
    for (const auto& axis : axes)
    {
        auto column = numbers(axis, width);
        const auto norm = std::inner_product(column.begin(), column.end(), column.begin(), 0.0);
        if (!std::isfinite(norm) || std::abs(norm - 1.0) > 1e-8) invalid();
        decoded.axes.push_back(std::move(column));
    }
    return decoded;
}
}

Json fit_pca(Rows train, const Json& options, const sql::WorkCheck& check)
{
    if (train.size() < 2 || train.front().empty() || !options.is_object() || options.size() != 1) invalid();
    const auto width = train.front().size();
    if (train.size() > row_limit || width > width_limit) limit();
    const auto count = nonnegative(field(options, "components"));
    if (!count || count > width) invalid();
    auto centered = center_rows(train, check);
    auto symmetric = covariance(centered, check);
    const auto vectors = eigenvectors(symmetric, check);
    std::vector<size_t> order(width);
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](size_t left, size_t right) {
        return symmetric[left][left] > symmetric[right][right];
    });

    double total{};
    for (size_t i = 0; i < width; ++i) total += (std::max)(0.0, symmetric[i][i]);
    std::vector<double> centers;
    centers.reserve(width);
    for (size_t i = 0; i < width; ++i) centers.push_back(centered.origin[i] + centered.offset[i]);
    auto axes = Json::array();
    auto explained = Json::array();
    auto ratios = Json::array();
    for (size_t i = 0; i < count; ++i)
    {
        const auto index = order[i];
        const auto variance = (std::max)(0.0, symmetric[index][index]);
        axes.push_back(oriented_axis(vectors, index));
        explained.push_back(variance * centered.scale * centered.scale);
        ratios.push_back(total > 0 ? Json(variance / total) : Json(nullptr));
    }
    return {{"format", 1}, {"width", width}, {"center", std::move(centers)}, {"origin", std::move(centered.origin)},
        {"mean_offset", std::move(centered.offset)}, {"components", std::move(axes)},
        {"explained_variance", std::move(explained)}, {"explained_variance_ratio", std::move(ratios)},
        {"total_variance", total * centered.scale * centered.scale},
        {"variance_reason", total > 0 ? Json(nullptr) : Json("zero_total_variance")}};
}

std::vector<double> transform_pca(const Json& artifact, std::span<const double> row)
{
    const auto model = read_artifact(artifact);
    if (row.size() != model.origin.size() || !std::ranges::all_of(row, valid_feature)) invalid();
    std::vector<double> scores;
    scores.reserve(model.axes.size());
    for (const auto& axis : model.axes)
    {
        double value{};
        for (size_t i = 0; i < row.size(); ++i)
            value = std::fma((row[i] - model.origin[i]) - model.offset[i], axis[i], value);
        if (!std::isfinite(value)) invalid();
        scores.push_back(value);
    }
    return scores;
}

std::vector<double> reconstruct_pca(const Json& artifact, std::span<const double> scores)
{
    const auto model = read_artifact(artifact);
    if (scores.size() != model.axes.size() || !std::ranges::all_of(scores, [](double v) { return std::isfinite(v); }))
        invalid();
    auto original = model.offset;
    for (size_t i = 0; i < original.size(); ++i)
    {
        for (size_t j = 0; j < scores.size(); ++j)
            original[i] = std::fma(scores[j], model.axes[j][i], original[i]);
        original[i] += model.origin[i];
        if (!std::isfinite(original[i])) invalid();
    }
    return original;
}
}
