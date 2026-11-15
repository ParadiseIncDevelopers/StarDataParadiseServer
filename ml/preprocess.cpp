#include "preprocess.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <random>
#include <set>

namespace sdp::ml
{
namespace
{
using wire::Json;
using W = wire::Code;
using Check = std::function<void()>;
using Index = uint32_t;

constexpr size_t row_limit = 1024;
constexpr size_t column_limit = 128;
constexpr size_t feature_width_limit = 256;
constexpr size_t category_limit = 64;
constexpr size_t text_limit = 4096;
constexpr double value_limit = 1e100;

enum class SplitMode { random, stratified, time };
enum class Transform { numeric, standard, normalize, onehot };
enum class Impute { reject, mean, zero, mode };

constexpr std::array transform_names{std::string_view{"numeric"}, std::string_view{"standard"},
    std::string_view{"normalize"}, std::string_view{"onehot"}};
constexpr std::array impute_names{std::string_view{"reject"}, std::string_view{"mean"},
    std::string_view{"zero"}, std::string_view{"mode"}};

[[noreturn]] void invalid() { throw wire::Error(W::invalid_argument); }
[[noreturn]] void too_large() { throw wire::Error(W::resource_limit); }

void shape(const Json& input, std::initializer_list<std::string_view> allowed)
{
    if (!input.is_object()) invalid();
    for (const auto& field : input.items())
        if (std::ranges::find(allowed, field.key()) == allowed.end()) invalid();
}

const Json& get(const Json& object, const char* key)
{
    const auto found = object.find(key);
    if (found == object.end()) invalid();
    return *found;
}

const std::string& string(const Json& value)
{
    if (!value.is_string()) invalid();
    return value.get_ref<const std::string&>();
}

const Json& array(const Json& value)
{
    if (!value.is_array()) invalid();
    return value;
}

uint64_t unsigned_number(const Json& value)
{
    if (value.is_number_unsigned()) return value.get<uint64_t>();
    if (!value.is_number_integer()) invalid();
    const auto signed_value = value.get<int64_t>();
    if (signed_value < 0) invalid();
    return static_cast<uint64_t>(signed_value);
}

uint64_t read_unsigned(const Json& object, const char* key, uint64_t fallback)
{
    return object.contains(key) ? unsigned_number(object.at(key)) : fallback;
}

int64_t time_value(const Json& value)
{
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() && value.get<uint64_t>() > uint64_t{INT64_MAX}))
        invalid();
    return value.get<int64_t>();
}

double number(const Json& value)
{
    if (!value.is_number()) invalid();
    const auto result = value.get<double>();
    if (!std::isfinite(result)) invalid();
    return result;
}

double numeric(const Json& value)
{
    if (value.is_boolean()) return value.get<bool>() ? 1.0 : 0.0;
    const auto result = number(value);
    if (std::abs(result) > value_limit) invalid();
    return result;
}

void validate_scalar(const Json& value)
{
    if (value.is_null() || value.is_boolean()) return;
    if (value.is_string())
    {
        if (string(value).size() > text_limit) too_large();
        return;
    }
    (void)numeric(value);
}

std::string category_key(const Json& value)
{
    if (value.is_null()) invalid();
    validate_scalar(value);
    if (value.is_number_float() && value.get<double>() == 0) return "0.0";
    try { return value.dump(); }
    catch (const Json::exception&) { invalid(); }
}

template<class Enum, size_t Count>
Enum option(const Json& value, const std::array<std::string_view, Count>& keys)
{
    const auto& name = string(value);
    const auto found = std::ranges::find(keys, name);
    if (found == keys.end()) invalid();
    return static_cast<Enum>(found - keys.begin());
}

struct Source
{
    const Json& names;
    const Json& rows;

    size_t column(const Json& value) const
    {
        const auto& name = string(value);
        const auto found = std::find_if(names.begin(), names.end(), [&](const Json& item) {
            return string(item) == name;
        });
        if (found == names.end()) invalid();
        return static_cast<size_t>(found - names.begin());
    }
};

Source read_source(const Json& source, const Check& check)
{
    shape(source, {"columns", "rows"});
    const auto& names = array(get(source, "columns"));
    const auto& rows = array(get(source, "rows"));
    if (names.empty() || rows.size() < 3) invalid();
    if (names.size() > column_limit || rows.size() > row_limit) too_large();
    std::set<std::string_view> unique;
    for (const auto& name : names)
    {
        const auto& label = string(name);
        if (label.empty() || label.size() > 128 || label.find('\0') != label.npos || !unique.insert(label).second)
            invalid();
    }
    for (const auto& row : rows)
    {
        if (check) check();
        if (!row.is_array() || row.size() != names.size()) invalid();
        for (const auto& value : row) validate_scalar(value);
    }
    return {names, rows};
}

struct SplitSpec
{
    SplitMode mode;
    uint64_t seed;
    size_t train;
    size_t validation;
    std::optional<size_t> group;
    std::optional<size_t> time;
};

SplitSpec read_split(const Source& source, const Json& input)
{
    shape(input, {"mode", "seed", "train", "validation", "group", "time"});
    const auto mode = option<SplitMode>(get(input, "mode"),
        std::array{std::string_view{"random"}, std::string_view{"stratified"}, std::string_view{"time"}});
    const auto train = read_unsigned(input, "train", 60);
    const auto validation = read_unsigned(input, "validation", 20);
    if (!train || !validation || train > 98 || validation > 98 || train + validation >= 100) invalid();
    if (input.contains("time") != (mode == SplitMode::time)) invalid();
    SplitSpec split{mode, read_unsigned(input, "seed", 0), static_cast<size_t>(train),
        static_cast<size_t>(validation), {}, {}};
    if (input.contains("group")) split.group = source.column(input.at("group"));
    if (input.contains("time")) split.time = source.column(input.at("time"));
    return split;
}

struct Unit
{
    std::vector<Index> rows;
    std::string label;
    int64_t first{INT64_MAX};
    int64_t last{INT64_MIN};
};

std::vector<Unit> split_units(const Source& source, const SplitSpec& spec, std::optional<size_t> stratify)
{
    std::map<std::string, size_t, std::less<>> groups;
    std::vector<Unit> result;
    for (Index row = 0; row < source.rows.size(); ++row)
    {
        const auto& values = source.rows[row];
        size_t unit_index = result.size();
        if (spec.group)
            unit_index = groups.try_emplace(category_key(values[*spec.group]), unit_index).first->second;
        if (unit_index == result.size()) result.emplace_back();
        auto& unit = result[unit_index];
        if (stratify)
        {
            const auto label = category_key(values[*stratify]);
            if (unit.rows.empty()) unit.label = label;
            else if (unit.label != label) invalid();
        }
        unit.rows.push_back(row);
        if (spec.time)
        {
            const auto time = time_value(values[*spec.time]);
            unit.first = (std::min)(unit.first, time);
            unit.last = (std::max)(unit.last, time);
        }
    }
    return result;
}

std::vector<Unit> merge_time_units(std::vector<Unit> units)
{
    std::stable_sort(units.begin(), units.end(), [](const Unit& left, const Unit& right) {
        return left.first < right.first;
    });
    std::vector<Unit> result;
    for (auto& unit : units)
    {
        if (result.empty() || unit.first > result.back().last)
        {
            result.push_back(std::move(unit));
            continue;
        }
        auto& block = result.back();
        block.last = (std::max)(block.last, unit.last);
        block.rows.insert(block.rows.end(), unit.rows.begin(), unit.rows.end());
    }
    return result;
}

struct Splits
{
    std::vector<Index> train;
    std::vector<Index> validation;
    std::vector<Index> test;
};

void copy_units(std::vector<Index>& partition, const std::vector<Unit>& units, std::span<const size_t> selected)
{
    for (const auto unit : selected)
        partition.insert(partition.end(), units[unit].rows.begin(), units[unit].rows.end());
}

size_t rounded_share(size_t count, size_t part, size_t whole)
{
    return (count * part + whole / 2) / whole;
}

Splits stratified_split(const std::vector<Unit>& units, const SplitSpec& spec, std::mt19937_64& random)
{
    std::map<std::string_view, std::vector<size_t>, std::less<>> strata;
    for (size_t i = 0; i < units.size(); ++i) strata[units[i].label].push_back(i);
    const auto test_share = 100 - spec.train - spec.validation;
    size_t validation_units{}, test_units{};
    Splits result;
    for (auto& [label, members] : strata)
    {
        std::shuffle(members.begin(), members.end(), random);
        const auto n = members.size();
        const auto selected = std::span<const size_t>{members};
        const auto train_maximum = n >= 3 ? n - 2 : 1;
        const auto n_train = std::clamp(rounded_share(n, spec.train, 100), size_t{1}, train_maximum);
        const auto remaining = n - n_train;
        size_t n_validation{};
        if (remaining == 1)
            n_validation = validation_units * test_share <= test_units * spec.validation ? 1 : 0;
        else if (remaining > 1)
            n_validation = std::clamp(rounded_share(remaining, spec.validation, 100 - spec.train),
                size_t{1}, remaining - 1);
        copy_units(result.train, units, selected.first(n_train));
        copy_units(result.validation, units, selected.subspan(n_train, n_validation));
        copy_units(result.test, units, selected.subspan(n_train + n_validation));
        validation_units += n_validation;
        test_units += remaining - n_validation;
    }
    return result;
}

Splits partition_rows(const Source& source, const SplitSpec& spec, std::optional<size_t> target)
{
    auto units = split_units(source, spec, spec.mode == SplitMode::stratified ? target : std::nullopt);
    if (spec.mode == SplitMode::time) units = merge_time_units(std::move(units));
    if (units.size() < 3) invalid();
    std::mt19937_64 random(spec.seed);
    if (spec.mode == SplitMode::stratified)
    {
        auto result = stratified_split(units, spec, random);
        if (result.validation.empty() || result.test.empty()) invalid();
        return result;
    }

    std::vector<size_t> order(units.size());
    std::iota(order.begin(), order.end(), size_t{0});
    if (spec.mode == SplitMode::random) std::shuffle(order.begin(), order.end(), random);
    const auto n_train = std::clamp(rounded_share(units.size(), spec.train, 100), size_t{1}, units.size() - 2);
    const auto n_validation = std::clamp(rounded_share(units.size(), spec.validation, 100),
        size_t{1}, units.size() - n_train - 1);
    const auto selected = std::span<const size_t>{order};
    Splits result;
    copy_units(result.train, units, selected.first(n_train));
    copy_units(result.validation, units, selected.subspan(n_train, n_validation));
    copy_units(result.test, units, selected.subspan(n_train + n_validation));
    if (spec.time)
    {
        const auto compare = [&](Index left, Index right) {
            const auto a = time_value(source.rows[left][*spec.time]);
            const auto b = time_value(source.rows[right][*spec.time]);
            return a == b ? left < right : a < b;
        };
        std::ranges::sort(result.train, compare);
        std::ranges::sort(result.validation, compare);
        std::ranges::sort(result.test, compare);
    }
    return result;
}

struct Feature
{
    std::string column;
    Transform transform;
    Impute impute;
};

Feature read_feature(const Json& input)
{
    const auto& column = string(get(input, "column"));
    const auto transform = option<Transform>(get(input, "transform"), transform_names);
    const auto impute = option<Impute>(get(input, "impute"), impute_names);
    if (column.empty() || column.size() > 128 || column.find('\0') != column.npos) invalid();
    if (transform == Transform::onehot && (impute == Impute::mean || impute == Impute::zero)) invalid();
    return {column, transform, impute};
}

struct Fitted
{
    Feature spec;
    Json fill = nullptr;
    double offset{};
    double scale{1.0};
    std::vector<Json> categories;
    std::vector<std::string> category_keys;
    size_t width() const { return spec.transform == Transform::onehot ? categories.size() + 1 : 1; }
};

struct Moments
{
    size_t count{};
    double mean{};
    double deviations{};
    double low{std::numeric_limits<double>::infinity()};
    double high{-std::numeric_limits<double>::infinity()};

    void add(double value)
    {
        low = (std::min)(low, value);
        high = (std::max)(high, value);
        const auto delta = value - mean;
        ++count;
        mean += delta / static_cast<double>(count);
        deviations += delta * (value - mean);
    }
};

struct Category { Json value; size_t count{}; };
using Categories = std::map<std::string, Category, std::less<>>;

void count_category(Categories& categories, const Json& value)
{
    auto& entry = categories.try_emplace(category_key(value), Category{value}).first->second;
    ++entry.count;
    if (categories.size() > category_limit) too_large();
}

Fitted fit(Feature feature, const Source& source, size_t column, const Splits& split, const Check& check)
{
    Fitted fitted{std::move(feature)};
    const bool onehot = fitted.spec.transform == Transform::onehot;
    Moments observed;
    Categories categories;
    for (const auto row : split.train)
    {
        if (check) check();
        const auto& cell = source.rows[row][column];
        if (cell.is_null())
        {
            if (fitted.spec.impute == Impute::reject) invalid();
            continue;
        }
        if (!onehot) observed.add(numeric(cell));
        if (onehot || fitted.spec.impute == Impute::mode) count_category(categories, cell);
    }
    if (onehot ? categories.empty() : observed.count == 0) invalid();
    if (fitted.spec.impute == Impute::mean) fitted.fill = observed.mean;
    else if (fitted.spec.impute == Impute::zero) fitted.fill = 0.0;
    else if (fitted.spec.impute == Impute::mode)
    {
        const auto most_frequent = std::ranges::max_element(categories, {}, [](const auto& entry) {
            return entry.second.count;
        });
        fitted.fill = most_frequent->second.value;
    }
    if (onehot)
    {
        for (const auto& [key, entry] : categories)
        {
            fitted.category_keys.push_back(key);
            fitted.categories.push_back(entry.value);
        }
    }
    else if (fitted.spec.transform != Transform::numeric)
    {
        Moments scaled;
        for (const auto row : split.train)
        {
            if (check) check();
            const auto& cell = source.rows[row][column];
            scaled.add(numeric(cell.is_null() ? fitted.fill : cell));
        }
        if (fitted.spec.transform == Transform::standard)
        {
            fitted.offset = scaled.mean;
            fitted.scale = std::sqrt((std::max)(0.0, scaled.deviations) / static_cast<double>(scaled.count));
        }
        else
        {
            fitted.offset = scaled.low;
            fitted.scale = scaled.high - scaled.low;
        }
        if (fitted.scale == 0.0) fitted.scale = 1.0;
    }
    return fitted;
}

Json save_feature(const Fitted& fitted)
{
    return {{"column", fitted.spec.column}, {"transform", transform_names[static_cast<size_t>(fitted.spec.transform)]},
        {"impute", impute_names[static_cast<size_t>(fitted.spec.impute)]}, {"fill", fitted.fill},
        {"offset", fitted.offset}, {"scale", fitted.scale}, {"categories", fitted.categories}};
}

Fitted load_feature(const Json& encoded)
{
    shape(encoded, {"column", "transform", "impute", "fill", "offset", "scale", "categories"});
    Fitted fitted{read_feature(encoded)};
    fitted.fill = get(encoded, "fill");
    fitted.offset = number(get(encoded, "offset"));
    fitted.scale = number(get(encoded, "scale"));
    const auto& categories = array(get(encoded, "categories"));
    if (categories.size() > category_limit) too_large();
    if (fitted.scale <= 0 || fitted.fill.is_null() != (fitted.spec.impute == Impute::reject)) invalid();
    if (fitted.spec.transform == Transform::onehot)
    {
        if (categories.empty() || fitted.scale != 1 || fitted.offset != 0) invalid();
        std::set<std::string, std::less<>> unique;
        for (const auto& entry : categories)
        {
            const auto key = category_key(entry);
            if (!unique.insert(key).second) invalid();
            fitted.category_keys.push_back(key);
            fitted.categories.push_back(entry);
        }
        if (!fitted.fill.is_null() && !unique.contains(category_key(fitted.fill))) invalid();
    }
    else
    {
        if (!categories.empty()) invalid();
        if (!fitted.fill.is_null()) (void)numeric(fitted.fill);
        if (fitted.spec.impute == Impute::zero && number(fitted.fill) != 0) invalid();
        if (fitted.spec.transform == Transform::numeric && (fitted.scale != 1 || fitted.offset != 0)) invalid();
    }
    return fitted;
}

void emit(std::vector<double>& output, const Fitted& fitted, const Json& cell)
{
    const auto& value = cell.is_null() ? fitted.fill : cell;
    if (value.is_null()) invalid();
    if (fitted.spec.transform == Transform::onehot)
    {
        const auto key = category_key(value);
        const auto selected = static_cast<size_t>(std::ranges::find(fitted.category_keys, key) - fitted.category_keys.begin());
        const auto start = output.size();
        output.resize(start + fitted.width(), 0.0);
        output[start + selected] = 1.0;
        return;
    }
    const auto transformed = (numeric(value) - fitted.offset) / fitted.scale;
    if (!std::isfinite(transformed)) invalid();
    output.push_back(transformed);
}

Json prepare_target(const Source& source, size_t target, bool categorical, const Splits& split, Json& classes,
    const Check& check)
{
    auto values = Json::array();
    std::map<std::string, size_t, std::less<>> category_ids;
    if (categorical)
    {
        Categories seen;
        for (const auto row : split.train) count_category(seen, source.rows[row][target]);
        for (const auto& [key, entry] : seen)
        {
            category_ids.emplace(key, classes.size());
            classes.push_back(entry.value);
        }
    }
    for (const auto& row : source.rows)
    {
        if (check) check();
        const auto& value = row[target];
        if (!categorical)
        {
            values.push_back(numeric(value));
            continue;
        }
        const auto id = category_ids.find(category_key(value));
        if (id == category_ids.end()) invalid();
        values.push_back(id->second);
    }
    return values;
}
}

Json prepare_dataset(const Json& dataset, const Json& options, const std::function<void()>& check)
{
    const auto source = read_source(dataset, check);
    shape(options, {"features", "target", "target_kind", "split"});
    const auto spec = read_split(source, get(options, "split"));
    std::optional<size_t> target;
    bool categorical{};
    if (options.contains("target"))
    {
        target = source.column(options.at("target"));
        if (options.contains("target_kind"))
        {
            const auto& kind = string(options.at("target_kind"));
            if (kind != "numeric" && kind != "categorical") invalid();
            categorical = kind == "categorical";
        }
    }
    else if (options.contains("target_kind")) invalid();
    if (spec.mode == SplitMode::stratified && (!target || !categorical)) invalid();

    const auto& features = array(get(options, "features"));
    if (features.empty()) invalid();
    if (features.size() > column_limit) too_large();
    const auto split = partition_rows(source, spec, target);
    struct Selected { size_t column; Fitted fitted; };
    std::vector<Selected> compiled;
    std::set<size_t> included;
    auto saved = Json::array();
    size_t width{};
    for (const auto& request : features)
    {
        shape(request, {"column", "transform", "impute"});
        auto feature = read_feature(request);
        const auto index = source.column(get(request, "column"));
        if (target == index || !included.insert(index).second) invalid();
        auto fitted = fit(std::move(feature), source, index, split, check);
        width += fitted.width();
        if (width > feature_width_limit) too_large();
        saved.push_back(save_feature(fitted));
        compiled.push_back({index, std::move(fitted)});
    }
    auto dense = Json::array();
    for (const auto& row : source.rows)
    {
        if (check) check();
        std::vector<double> values;
        values.reserve(width);
        for (const auto& feature : compiled) emit(values, feature.fitted, row[feature.column]);
        dense.push_back(std::move(values));
    }
    auto classes = Json::array();
    auto labels = target ? prepare_target(source, *target, categorical, split, classes, check) : Json::array();
    return {{"format", 1},
        {"split", {{"train", split.train}, {"validation", split.validation}, {"test", split.test}}},
        {"transforms", {{"format", 1}, {"width", width}, {"features", std::move(saved)}}},
        {"features", std::move(dense)}, {"target", std::move(labels)}, {"target_classes", std::move(classes)}};
}

std::vector<double> transform_features(const Json& transforms, const Json& named_features)
{
    shape(transforms, {"format", "width", "features"});
    if (unsigned_number(get(transforms, "format")) != 1 || !named_features.is_object()) invalid();
    const auto width = unsigned_number(get(transforms, "width"));
    const auto& features = array(get(transforms, "features"));
    if (features.empty() || !width || named_features.size() != features.size()) invalid();
    if (width > feature_width_limit || features.size() > column_limit) too_large();
    std::set<std::string> used;
    std::vector<double> result;
    result.reserve(static_cast<size_t>(width));
    for (const auto& encoded : features)
    {
        const auto fitted = load_feature(encoded);
        if (!used.insert(fitted.spec.column).second || result.size() + fitted.width() > width) invalid();
        emit(result, fitted, get(named_features, fitted.spec.column.c_str()));
    }
    if (result.size() != width) invalid();
    return result;
}
}
