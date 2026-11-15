#include "preprocess.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace
{
using sdp::ml::prepare_dataset;
using sdp::ml::transform_features;
using sdp::wire::Code;
using sdp::wire::Json;

void require(bool accepted, const char* message)
{
    if (!accepted) throw std::runtime_error(message);
}

template<class Function>
void expect(Code code, const Function& function)
{
    try { function(); }
    catch (const sdp::wire::Error& error)
    {
        require(error.code() == code, "unexpected preprocessing status");
        return;
    }
    throw std::runtime_error("expected a preprocessing error");
}

Json feature(const char* column, const char* transform, const char* impute)
{
    return {{"column", column}, {"transform", transform}, {"impute", impute}};
}

Json random_source()
{
    auto rows = Json::array();
    for (unsigned group = 0; group < 12; ++group)
        for (unsigned part = 0; part < 2; ++part)
        {
            const auto value = group * 2 + part;
            rows.push_back(Json::array({part == 0 ? Json(nullptr) : Json(value), group,
                group % 2 == 0, part ? "red" : "blue"}));
        }
    return {{"columns", {"x", "family", "label", "color"}}, {"rows", std::move(rows)}};
}

Json random_options()
{
    return {{"features", {feature("x", "standard", "mean"), feature("color", "onehot", "mode")}},
        {"split", {{"mode", "random"}, {"seed", 90210}, {"group", "family"}}}};
}

void partition_integrity(const Json& source, const Json& prepared, size_t group)
{
    const auto& rows = source.at("rows");
    std::set<size_t> all;
    std::map<std::string, std::string> owners;
    for (const auto name : {"train", "validation", "test"})
    {
        const auto& indices = prepared.at("split").at(name);
        require(!indices.empty(), "empty partition");
        for (const auto& index : indices)
        {
            const auto position = index.get<size_t>();
            require(position < rows.size() && all.insert(position).second, "split shares a row or uses an invalid index");
            const auto [entry, created] = owners.try_emplace(rows[position][group].dump(), name);
            require(created || entry->second == name, "split shares a group");
        }
    }
    require(all.size() == rows.size(), "split failed to cover every source row");
}

void test_leakage()
{
    auto source = random_source();
    const auto options = random_options();
    const auto original = prepare_dataset(source, options);
    partition_integrity(source, original, 1);
    require(prepare_dataset(source, options) == original, "same seed and snapshot produced different splits or transforms");
    const auto& train = original.at("split").at("train");
    double observed_sum{};
    size_t observed{};
    for (const auto& entry : train)
    {
        const auto& value = source["rows"][entry.get<size_t>()][0];
        if (!value.is_null())
        {
            observed_sum += value.get<double>();
            ++observed;
        }
    }
    const auto fill = original.at("transforms").at("features").at(0).at("fill").get<double>();
    require(observed && std::abs(fill - observed_sum / static_cast<double>(observed)) < 1e-12,
        "mean imputation differs from the training-only reference");

    const auto held_out = original.at("split").at("test").at(0).get<size_t>();
    source["rows"][held_out][0] = 1e12;
    source["rows"][held_out][3] = "held-out-only";
    const auto changed = prepare_dataset(source, options);
    require(original.at("split") == changed.at("split") && original.at("transforms") == changed.at("transforms"),
        "a held-out outlier or category affected training-only fit");
    for (const auto& entry : train)
        require(original.at("features").at(entry.get<size_t>()) == changed.at("features").at(entry.get<size_t>()),
            "held-out edit changed a training vector");
    const auto prediction = transform_features(changed.at("transforms"), {{"x", 1e12}, {"color", "held-out-only"}});
    require(prediction == changed.at("features").at(held_out).get<std::vector<double>>() &&
        prediction.size() == 4 && prediction.back() == 1 && prediction[0] > 1e10,
        "inference refit transforms, clipped held-out data or lost the unknown category slot");

    auto corrupted = original.at("transforms");
    corrupted["features"][0]["scale"] = 0.0;
    expect(Code::invalid_argument, [&] { (void)transform_features(corrupted, {{"x", 5}, {"color", "blue"}}); });
    expect(Code::invalid_argument, [&] {
        (void)transform_features(original.at("transforms"), {{"x", 5}, {"unexpected", "blue"}});
    });
    corrupted = original.at("transforms");
    corrupted["features"][0]["column"] = std::string("x\0extra", 7);
    expect(Code::invalid_argument, [&] { (void)transform_features(corrupted, {{"x", 5}, {"color", "blue"}}); });
    expect(Code::cancelled, [&] {
        (void)prepare_dataset(source, options, [] { throw sdp::wire::Error(Code::cancelled); });
    });
}

void test_stratified()
{
    auto source = random_source();
    Json options{{"features", {feature("x", "normalize", "mean")}},
        {"target", "label"}, {"target_kind", "categorical"},
        {"split", {{"mode", "stratified"}, {"seed", 71}, {"group", "family"}}}};
    const auto prepared = prepare_dataset(source, options);
    partition_integrity(source, prepared, 1);
    require(prepared.at("target_classes") == Json::array({false, true}), "boolean class dictionary is not stable");
    for (const auto key : {"train", "validation", "test"})
    {
        std::set<unsigned> labels;
        for (const auto& index : prepared.at("split").at(key))
            labels.insert(prepared.at("target").at(index.get<size_t>()).get<unsigned>());
        require(labels == std::set<unsigned>{0, 1}, "stratified split lost an available class");
    }
    options["features"] = Json::array({feature("label", "numeric", "reject")});
    expect(Code::invalid_argument, [&] { (void)prepare_dataset(source, options); });
    options["features"] = Json::array({feature("x", "numeric", "mean")});
    source["rows"][0][2] = false;
    expect(Code::invalid_argument, [&] { (void)prepare_dataset(source, options); });
}

void test_time()
{
    Json source{{"columns", {"x", "family", "epoch"}}, {"rows", {
        {900, "last", INT64_C(9007199254741008)}, {20, "early-span", INT64_C(9007199254740995)},
        {15, "inside-span", INT64_C(9007199254740994)}, {12, "same-time", INT64_C(9007199254740995)},
        {5, "early-span", INT64_C(9007199254740993)}, {40, "later", INT64_C(9007199254741000)},
        {80, "after", INT64_C(9007199254741003)}, {60, "middle", INT64_C(9007199254741001)}
    }}};
    Json options{{"features", {feature("x", "normalize", "reject")}},
        {"split", {{"mode", "time"}, {"group", "family"}, {"time", "epoch"}, {"train", 40}, {"validation", 30}}}};
    const auto prepared = prepare_dataset(source, options);
    partition_integrity(source, prepared, 1);
    const auto& split = prepared.at("split");
    int64_t previous = INT64_MIN;
    for (const auto key : {"train", "validation", "test"})
    {
        std::vector<int64_t> times;
        for (const auto& index : split.at(key))
            times.push_back(source.at("rows").at(index.get<size_t>()).at(2).get<int64_t>());
        require(std::ranges::is_sorted(times) && previous < times.front(), "time split leaked future or tied samples");
        previous = times.back();
    }
    require(prepared.at("transforms").at("features").at(0).at("offset") == 5.0 &&
        prepared.at("transforms").at("features").at(0).at("scale") == 35.0,
        "time split fit used future rows");
    for (auto& row : source["rows"]) row[2] = 10;
    expect(Code::invalid_argument, [&] { (void)prepare_dataset(source, options); });
}

void test_rejections()
{
    auto source = random_source();
    auto options = random_options();
    options["target"] = "label";
    options["target_kind"] = "categorical";
    const auto base = prepare_dataset(source, options);
    source["rows"][base.at("split").at("test").at(0).get<size_t>()][2] = "unknown-class";
    expect(Code::invalid_argument, [&] { (void)prepare_dataset(source, options); });
    source = random_source();
    source["rows"][0][0] = (std::numeric_limits<double>::infinity)();
    expect(Code::invalid_argument, [&] { (void)prepare_dataset(source, options); });
    source = random_source();
    options["split"]["train"] = 100;
    expect(Code::invalid_argument, [&] { (void)prepare_dataset(source, options); });
    const auto sample = source["rows"][0];
    for (unsigned i = 0; i < 1024; ++i) source["rows"].push_back(sample);
    expect(Code::resource_limit, [&] { (void)prepare_dataset(source, options); });
}
}

int main()
{
    try
    {
        test_leakage();
        test_stratified();
        test_time();
        test_rejections();
        std::cout << "Dataset train-only fit, inference reuse, disjoint grouped splits, stratification, time ordering and limits passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
