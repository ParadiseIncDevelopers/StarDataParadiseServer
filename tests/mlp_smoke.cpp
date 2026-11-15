#include "mlp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>

namespace
{
using namespace sdp;
using wire::Json;
using W = wire::Code;
using Rows = std::vector<std::vector<double>>;

void require(bool passed, const char* reason)
{
    if (!passed) throw std::runtime_error(reason);
}

template<class Operation>
void reject(Operation operation, W code, const char* reason)
{
    try { operation(); }
    catch (const wire::Error& error)
    {
        require(error.code() == code, reason);
        return;
    }
    throw std::runtime_error(reason);
}

void finite_difference()
{
    const std::array<uint32_t, 3> layers{2, 2, 1};
    const std::array weights{0.2, -0.4, 0.1, 0.5, 0.3, -0.2, 0.35, -0.45, 0.05};
    const Rows rows{{0.7, -0.3}, {-0.8, 0.33}, {0.25, 0.6}};
    const std::array labels{1.0, 0.0, 1.0};
    // Independent logit-space BCE reference, mean over the three rows.
    const std::array expected{-0.09734799948778712, -0.0005721667652119067, -0.04476530943449309,
        0.11901747310960915, 0.0030251560726735787, 0.08003389871313422,
        -0.07286835707700752, -0.11148645162832067, -0.14112895359948294};
    const auto actual = ml::mlp_loss_gradient(layers, "tanh", weights, rows, labels);
    require(std::abs(actual.loss - 0.7083341400897236) < 1e-12 && actual.gradient.size() == weights.size(),
        "Forward BCE or gradient width differs from independent reference");
    constexpr double h = 1e-6;
    for (size_t index = 0; index < weights.size(); ++index)
    {
        auto above = weights, below = weights;
        above[index] += h;
        below[index] -= h;
        const auto high = ml::mlp_loss_gradient(layers, "tanh", above, rows, labels).loss;
        const auto low = ml::mlp_loss_gradient(layers, "tanh", below, rows, labels).loss;
        const auto central = (high - low) / (2 * h);
        require(std::abs(actual.gradient[index] - expected[index]) < 1e-10 &&
            std::abs(actual.gradient[index] - central) < 1e-5, "MLP backprop disagrees with independent or central gradient");
    }
}

double independent_bce(const Json& artifact, ml::MlpRows rows, std::span<const double> labels)
{
    double sum{};
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const auto predicted = ml::predict_mlp(artifact, rows[i]);
        require(std::isfinite(predicted) && predicted > 0 && predicted < 1, "MLP inference produced a degenerate probability");
        sum -= labels[i] == 1 ? std::log(predicted) : std::log1p(-predicted);
    }
    return sum / static_cast<double>(rows.size());
}

struct Partition
{
    Rows x;
    std::vector<double> y;
};

Partition xor_rows(std::span<const std::array<double, 2>> offsets)
{
    Partition result;
    for (const auto left : {-1.0, 1.0})
        for (const auto right : {-1.0, 1.0})
            for (const auto& offset : offsets)
            {
                result.x.push_back({left + offset[0], right + offset[1]});
                result.y.push_back(left != right ? 1.0 : 0.0);
            }
    return result;
}

void nonlinear_fit()
{
    const std::array train_offsets{std::array{-0.2, -0.2}, std::array{0.2, 0.2},
        std::array{-0.2, 0.2}, std::array{0.2, -0.2}};
    const std::array<std::array<double, 2>, 1> validation_offsets{std::array{0.06, -0.04}};
    const std::array holdout_offsets{std::array{-0.13, 0.11}, std::array{0.17, -0.09}};
    const auto train = xor_rows(train_offsets);
    const auto validation = xor_rows(validation_offsets);
    const auto holdout = xor_rows(holdout_offsets);
    for (const auto& row : holdout.x)
        require(std::ranges::find(train.x, row) == train.x.end() &&
            std::ranges::find(validation.x, row) == validation.x.end(), "Held-out fixture overlapped training or validation");

    const Json options{{"hidden_layers", {8}}, {"activation", "tanh"}, {"optimizer", "adam"},
        {"learning_rate", 0.03}, {"batch_size", 4}, {"epochs", 220}};
    const auto artifact = ml::fit_mlp(train.x, train.y, validation.x, validation.y, options, 41);
    const auto reloaded = Json::parse(artifact.dump());
    require(artifact == ml::fit_mlp(train.x, train.y, validation.x, validation.y, options, 41),
        "Deterministic CPU training with identical seed produced different artifacts");
    require(artifact.at("training_loss").get<double>() < artifact.at("initial_training_loss").get<double>() * 0.4,
        "MLP did not learn nonlinear training labels");
    size_t hits{};
    for (size_t i = 0; i < holdout.x.size(); ++i)
        if ((ml::predict_mlp(reloaded, holdout.x[i]) >= 0.5) == (holdout.y[i] == 1)) ++hits;
    const auto loss = independent_bce(reloaded, holdout.x, holdout.y);
    require(hits == holdout.x.size() && loss < 0.2, "MLP failed the independent XOR holdout");
    std::cout << "XOR 8/8 held-out, BCE " << loss << " from initial training "
        << artifact.at("initial_training_loss").get<double>() << '\n';

    reject([&] { (void)ml::predict_mlp(artifact, std::array{1.0, 2.0, 3.0}); }, W::invalid_argument,
        "MLP accepted wrong input width");
    reject([&] { (void)ml::predict_mlp(artifact, std::array{std::numeric_limits<double>::quiet_NaN(), 0.0}); },
        W::invalid_argument, "MLP accepted a NaN input");
    auto invalid = artifact;
    invalid["parameters"][0] = 1e30;
    reject([&] { (void)ml::predict_mlp(invalid, holdout.x.at(0)); }, W::invalid_argument,
        "MLP accepted oversized persisted weights");
    auto bad_y = train.y;
    bad_y[0] = 2;
    reject([&] { (void)ml::fit_mlp(train.x, bad_y, validation.x, validation.y, options, 41); },
        W::invalid_argument, "MLP accepted a nonbinary training label");
    reject([&] { (void)ml::fit_mlp(train.x, train.y, validation.x, validation.y, options, 41,
        [] { throw wire::Error(W::cancelled); }); }, W::cancelled, "MLP ignored its work cancellation");
}

const Rows train_reversed{{-2,-1}, {-1.9,-1}, {2,1}, {1.9,1}};
const std::array reverse_train_y{0.0, 0.0, 1.0, 1.0};
const Rows validation_reversed{{-2.01,-1}, {-1.89,-1}, {2.01,1}, {1.89,1}};
const std::array reverse_val_y{1.0, 1.0, 0.0, 0.0};

void optimizer_step()
{
    for (const auto optimizer : {"sgd", "adam"})
    {
        constexpr double rate = 0.01;
        const Json options{{"hidden_layers", {2}}, {"activation", "tanh"}, {"optimizer", optimizer},
            {"learning_rate", rate}, {"batch_size", 4}, {"epochs", 1}};
        Json step;
        const auto restored = ml::fit_mlp(train_reversed, reverse_train_y, validation_reversed, reverse_val_y,
            options, 41, {}, {}, [&](const auto& saved) { step = saved; });
        require(!step.is_null() && restored.at("best_epoch") == 0 && step.at("optimizer_steps") == 1,
            "One-step optimizer reference did not retain its independent initial weights");
        const auto before = restored.at("parameters").get<std::vector<double>>();
        const auto after = step.at("weights").get<std::vector<double>>();
        const auto layers = restored.at("layers").get<std::vector<uint32_t>>();
        const auto grad = ml::mlp_loss_gradient(layers, "tanh", before, train_reversed, reverse_train_y).gradient;
        require(after.size() == grad.size(), "Unexpected optimizer step width");
        for (size_t i = 0; i < after.size(); ++i)
        {
            auto update = grad[i];
            if (std::string_view(optimizer) == "adam")
            {
                require(std::abs(step.at("first_moment")[i].get<double>() - 0.1 * update) < 1e-12 &&
                    std::abs(step.at("second_moment")[i].get<double>() - 0.001 * update * update) < 1e-12,
                    "Adam moment state disagrees with the first-step equations");
                update /= std::abs(update) + 1e-8;
            }
            require(std::abs(after[i] - (before[i] - rate * update)) < 1e-10, "Optimizer parameter update is incorrect");
        }
    }
}

void validation_restore()
{
    const Json options{{"hidden_layers", {2}}, {"activation", "tanh"}, {"optimizer", "adam"},
        {"learning_rate", 0.03}, {"batch_size", 4}, {"epochs", 20}, {"patience", 4}, {"min_delta", 1e-4}};
    Json last;
    std::vector<double> losses;
    const auto artifact = ml::fit_mlp(train_reversed, reverse_train_y, validation_reversed, reverse_val_y, options, 41,
        {}, {}, [&](const auto& saved) {
            last = saved;
            const Json candidate{{"format", 1}, {"loss", "bce"}, {"layers", saved.at("layers")},
                {"activation", "tanh"}, {"parameters", saved.at("weights")}};
            losses.push_back(independent_bce(candidate, validation_reversed, reverse_val_y));
        });
    require(!losses.empty() && artifact.at("early_stopped") == true &&
        artifact.at("epochs_completed").get<size_t>() == losses.size(),
        "MLP did not stop after its validation patience was exhausted");
    require(last.at("weights") != artifact.at("parameters") &&
        last.at("best_weights") == artifact.at("parameters"), "MLP returned last weights instead of validation-best weights");
    const auto best = (std::min)(*std::ranges::min_element(losses), artifact.at("initial_validation_loss").get<double>());
    require(std::abs(artifact.at("validation_loss").get<double>() - best) < 1e-12 &&
        std::abs(independent_bce(artifact, validation_reversed, reverse_val_y) - best) < 1e-12,
        "Restored MLP weights and persisted validation metric disagree with independently measured best epoch");
}
}

int main()
{
    try
    {
        finite_difference();
        nonlinear_fit();
        optimizer_step();
        validation_restore();
        std::cout << "Independent BCE gradients, SGD/Adam steps, held-out training, validation best, and invalid inputs passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
