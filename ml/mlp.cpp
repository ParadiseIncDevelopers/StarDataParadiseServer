#include "mlp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <random>
#include <utility>

namespace sdp::ml
{
namespace
{
using wire::Json;
using W = wire::Code;
using Vector = std::vector<double>;

constexpr uint32_t maximum_input_width = 64;
constexpr uint32_t maximum_hidden_width = 32;
constexpr size_t maximum_hidden_layers = 3;
constexpr size_t maximum_rows = 1024;
constexpr size_t maximum_parameters = 2048;
constexpr uint32_t maximum_epochs = 1000;
constexpr uint64_t maximum_operations = 300'000'000;
constexpr uint64_t maximum_rng_draws = 2'100'000;
constexpr double value_limit = 1e6;
constexpr double beta1 = 0.9;
constexpr double beta2 = 0.999;
constexpr double adam_epsilon = 1e-8;

enum class Activation { tanh, relu, sigmoid };
enum class Optimizer { adam, sgd };

constexpr std::array activations{std::string_view{"tanh"}, std::string_view{"relu"}, std::string_view{"sigmoid"}};
constexpr std::array optimizers{std::string_view{"adam"}, std::string_view{"sgd"}};

[[noreturn]] void invalid() { throw wire::Error(W::invalid_argument); }
[[noreturn]] void limit() { throw wire::Error(W::resource_limit); }

void poll(const sql::WorkCheck& check)
{
    if (check) check();
}

void shape(const Json& object, std::initializer_list<std::string_view> keys)
{
    if (!object.is_object()) invalid();
    for (const auto& item : object.items())
        if (std::ranges::find(keys, item.key()) == keys.end()) invalid();
}

const Json& field(const Json& object, const char* key)
{
    const auto found = object.find(key);
    if (found == object.end()) invalid();
    return *found;
}

const std::string& text(const Json& value)
{
    if (!value.is_string()) invalid();
    return value.get_ref<const std::string&>();
}

uint64_t unsigned_number(const Json& input)
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

double loss_value(const Json& value)
{
    const auto parsed = number(value);
    if (parsed < 0) invalid();
    return parsed;
}

bool bounded(double value)
{
    return std::isfinite(value) && std::abs(value) <= value_limit;
}

uint32_t positive(const Json& input, uint32_t maximum)
{
    const auto value = unsigned_number(input);
    if (value == 0 || value > maximum) invalid();
    return static_cast<uint32_t>(value);
}

uint32_t integer_option(const Json& object, const char* key, uint32_t fallback, uint32_t maximum)
{
    return object.contains(key) ? positive(object.at(key), maximum) : fallback;
}

double real_option(const Json& object, const char* key, double fallback, double minimum, double maximum)
{
    if (!object.contains(key)) return fallback;
    const auto value = number(object.at(key));
    if (value < minimum || value > maximum) invalid();
    return value;
}

template<class Enum, size_t Count>
Enum find_name(std::string_view value, const std::array<std::string_view, Count>& choices)
{
    const auto found = std::ranges::find(choices, value);
    if (found == choices.end()) invalid();
    return static_cast<Enum>(found - choices.begin());
}

Vector read_vector(const Json& value, size_t size)
{
    if (!value.is_array() || value.size() != size) invalid();
    Vector result;
    result.reserve(size);
    for (const auto& element : value) result.push_back(number(element));
    return result;
}

struct Settings
{
    std::vector<uint32_t> layers;
    Activation activation{Activation::tanh};
    Optimizer optimizer{Optimizer::adam};
    double learning_rate{0.01};
    uint32_t batch_size{16};
    uint32_t epochs{120};
    uint32_t patience{20};
    double min_delta{1e-6};
};

Settings settings(const Json& options, uint32_t input_width)
{
    shape(options, {"hidden_layers", "activation", "loss", "optimizer", "learning_rate", "batch_size",
        "epochs", "patience", "min_delta"});
    Settings result;
    result.layers.push_back(input_width);
    if (options.contains("hidden_layers"))
    {
        const auto& hidden = options.at("hidden_layers");
        if (!hidden.is_array() || hidden.empty() || hidden.size() > maximum_hidden_layers) invalid();
        for (const auto& width : hidden) result.layers.push_back(positive(width, maximum_hidden_width));
    }
    else result.layers.push_back(8);
    result.layers.push_back(1);
    if (options.contains("activation")) result.activation = find_name<Activation>(text(options.at("activation")), activations);
    if (options.contains("optimizer")) result.optimizer = find_name<Optimizer>(text(options.at("optimizer")), optimizers);
    if (options.contains("loss") && text(options.at("loss")) != "bce") invalid();
    result.learning_rate = real_option(options, "learning_rate", result.learning_rate, 1e-8, 1.0);
    result.batch_size = integer_option(options, "batch_size", result.batch_size, 256);
    result.epochs = integer_option(options, "epochs", result.epochs, maximum_epochs);
    result.patience = integer_option(options, "patience", result.patience, maximum_epochs);
    result.min_delta = real_option(options, "min_delta", result.min_delta, 0, 1.0);
    return result;
}

Json save_settings(const Settings& input)
{
    return {{"activation", activations[static_cast<size_t>(input.activation)]},
        {"optimizer", optimizers[static_cast<size_t>(input.optimizer)]}, {"loss", "bce"},
        {"learning_rate", input.learning_rate}, {"batch_size", input.batch_size},
        {"epochs", input.epochs}, {"patience", input.patience}, {"min_delta", input.min_delta}};
}

bool binary_label(double value) { return value == 0.0 || value == 1.0; }

void validate_data(MlpRows rows, std::span<const double> labels, size_t width,
    const sql::WorkCheck& check, bool both_classes = false)
{
    if (rows.empty() || rows.size() != labels.size()) invalid();
    if (rows.size() > maximum_rows) limit();
    for (size_t i = 0; i < rows.size(); ++i)
    {
        poll(check);
        if (rows[i].size() != width || !std::ranges::all_of(rows[i], bounded) || !binary_label(labels[i])) invalid();
    }
    if (both_classes && (std::ranges::find(labels, 0.0) == labels.end() ||
        std::ranges::find(labels, 1.0) == labels.end())) invalid();
}

double sigmoid(double value)
{
    if (value >= 0) return 1.0 / (1.0 + std::exp(-value));
    const auto scaled = std::exp(value);
    return scaled / (1.0 + scaled);
}

double logistic_loss(double logit, double label)
{
    const auto signed_error = label == 0 ? logit : -logit;
    return (std::max)(signed_error, 0.0) + std::log1p(std::exp(-std::abs(signed_error)));
}

double activate(Activation activation, double value)
{
    switch (activation)
    {
    case Activation::tanh: return std::tanh(value);
    case Activation::relu: return (std::max)(0.0, value);
    case Activation::sigmoid: return sigmoid(value);
    }
    invalid();
}

double derivative(Activation activation, double output)
{
    switch (activation)
    {
    case Activation::tanh: return 1.0 - output * output;
    case Activation::relu: return output > 0.0 ? 1.0 : 0.0;
    case Activation::sigmoid: return output * (1.0 - output);
    }
    invalid();
}

struct Layer
{
    size_t input;
    size_t output;
    size_t input_at;
    size_t output_at;
    size_t parameter_at;

    size_t neuron(size_t index) const { return parameter_at + index * (input + 1); }
};

class Network
{
public:
    Network(std::span<const uint32_t> widths, Activation activation) : activation_(activation)
    {
        if (widths.size() < 3 || widths.size() > maximum_hidden_layers + 2 || widths.back() != 1 ||
            widths.front() == 0 || widths.front() > maximum_input_width) invalid();
        values_ = widths.front();
        for (size_t index = 1; index < widths.size(); ++index)
        {
            const auto input = widths[index - 1];
            const auto output = widths[index];
            if (output == 0 || output > maximum_hidden_width) invalid();
            layers_.push_back({input, output, values_ - input, values_, parameters_});
            parameters_ += (static_cast<size_t>(input) + 1) * output;
            weights_ += static_cast<size_t>(input) * output;
            values_ += output;
            if (parameters_ > maximum_parameters) limit();
        }
    }

    size_t parameters() const { return parameters_; }
    size_t random_weights() const { return weights_; }
    size_t values() const { return values_; }
    std::span<const Layer> layers() const { return layers_; }

    double forward(std::span<const double> parameters, std::span<const double> row, std::span<double> frame) const
    {
        std::copy(row.begin(), row.end(), frame.begin());
        double logit{};
        for (const auto& layer : layers_)
        {
            const bool is_output = layer.output_at + layer.output == values_;
            for (size_t j = 0; j < layer.output; ++j)
            {
                const auto base = layer.neuron(j);
                double value = parameters[base + layer.input];
                for (size_t i = 0; i < layer.input; ++i)
                    value = std::fma(parameters[base + i], frame[layer.input_at + i], value);
                if (!std::isfinite(value)) limit();
                frame[layer.output_at + j] = is_output ? sigmoid(value) : activate(activation_, value);
                if (is_output) logit = value;
            }
        }
        return logit;
    }

    void backpropagate(std::span<const double> parameters, double label, std::span<const double> frame,
        std::span<double> delta, std::span<double> gradient) const
    {
        std::fill(delta.begin(), delta.end(), 0.0);
        delta.back() = frame.back() - label;
        for (auto it = layers_.rbegin(); it != layers_.rend(); ++it)
        {
            const auto& layer = *it;
            for (size_t j = 0; j < layer.output; ++j)
            {
                const auto base = layer.neuron(j);
                const auto error = delta[layer.output_at + j];
                for (size_t i = 0; i < layer.input; ++i)
                {
                    const auto hidden = layer.input_at + i;
                    gradient[base + i] = std::fma(error, frame[hidden], gradient[base + i]);
                    if (layer.input_at) delta[hidden] = std::fma(error, parameters[base + i], delta[hidden]);
                }
                gradient[base + layer.input] += error;
            }
            if (layer.input_at)
                for (size_t i = 0; i < layer.input; ++i)
                {
                    const auto input = layer.input_at + i;
                    delta[input] *= derivative(activation_, frame[input]);
                }
        }
    }

private:
    Activation activation_;
    std::vector<Layer> layers_;
    size_t parameters_{};
    size_t weights_{};
    size_t values_{};
};

class Random
{
public:
    explicit Random(uint64_t seed) : engine_(seed), seed_(seed) {}

    double unit() { return std::ldexp(static_cast<double>(draw() >> 11), -53); }

    size_t below(size_t bound)
    {
        const auto divisor = static_cast<uint64_t>(bound);
        const auto first = (uint64_t{0} - divisor) % divisor;
        for (;;)
        {
            const auto value = draw();
            if (value >= first) return static_cast<size_t>(value % divisor);
        }
    }

    uint64_t draws() const { return draws_; }
    uint64_t seed() const { return seed_; }

    void restore(uint64_t distance)
    {
        if (distance > maximum_rng_draws) invalid();
        engine_.seed(seed_);
        engine_.discard(distance);
        draws_ = distance;
    }

private:
    uint64_t draw()
    {
        if (draws_ == maximum_rng_draws) limit();
        ++draws_;
        return engine_();
    }

    std::mt19937_64 engine_;
    uint64_t seed_;
    uint64_t draws_{};
};

struct TrainingState
{
    explicit TrainingState(uint64_t seed) : random(seed) {}

    void consume(uint64_t cost)
    {
        if (cost > maximum_operations - used_operations) limit();
        used_operations += cost;
    }

    Random random;
    Vector weights;
    Vector best_weights;
    Vector first_moment;
    Vector second_moment;
    uint64_t used_operations{};
    uint64_t optimizer_steps{};
    uint32_t epoch{};
    uint32_t best_epoch{};
    uint32_t stale_epochs{};
    double patience_anchor{};
    double initial_training_loss{};
    double initial_validation_loss{};
    double best_training_loss{};
    double best_validation_loss{};
};

struct Partition
{
    MlpRows rows;
    std::span<const double> labels;
};

MlpGradient batch_gradient(const Network& model, const Vector& weights, Partition sample,
    std::span<const size_t> indices, const sql::WorkCheck& check, bool measure_loss = false)
{
    MlpGradient result{0, Vector(model.parameters())};
    Vector frame(model.values());
    Vector delta(model.values());
    for (const auto i : indices)
    {
        poll(check);
        const auto logit = model.forward(weights, sample.rows[i], frame);
        if (measure_loss) result.loss += logistic_loss(logit, sample.labels[i]);
        model.backpropagate(weights, sample.labels[i], frame, delta, result.gradient);
    }
    const auto divisor = static_cast<double>(indices.size());
    result.loss /= divisor;
    for (auto& value : result.gradient)
    {
        value /= divisor;
        if (!std::isfinite(value)) limit();
    }
    return result;
}

double dataset_loss(const Network& model, const Vector& weights, Partition sample, const sql::WorkCheck& check)
{
    double result{};
    Vector frame(model.values());
    for (size_t i = 0; i < sample.rows.size(); ++i)
    {
        poll(check);
        result += logistic_loss(model.forward(weights, sample.rows[i], frame), sample.labels[i]);
    }
    if (!std::isfinite(result)) limit();
    return result / static_cast<double>(sample.rows.size());
}

void update_weights(TrainingState& state, const Settings& input, std::span<const double> gradient)
{
    ++state.optimizer_steps;
    const bool adam = input.optimizer == Optimizer::adam;
    const auto step = static_cast<double>(state.optimizer_steps);
    const auto first_bias = adam ? 1.0 - std::pow(beta1, step) : 1.0;
    const auto second_bias = adam ? 1.0 - std::pow(beta2, step) : 1.0;
    for (size_t i = 0; i < gradient.size(); ++i)
    {
        const auto g = gradient[i];
        double update = g;
        if (adam)
        {
            auto& first = state.first_moment[i];
            auto& second = state.second_moment[i];
            first = beta1 * first + (1.0 - beta1) * g;
            second = beta2 * second + (1.0 - beta2) * g * g;
            if (!std::isfinite(first) || !std::isfinite(second)) limit();
            update = (first / first_bias) / (std::sqrt(second / second_bias) + adam_epsilon);
        }
        state.weights[i] -= input.learning_rate * update;
        if (!bounded(state.weights[i])) limit();
    }
}

TrainingState initial_state(const Network& model, const Settings& input, uint64_t seed,
    Partition train, Partition val, const sql::WorkCheck& check)
{
    TrainingState state(seed);
    state.weights.resize(model.parameters());
    if (input.optimizer == Optimizer::adam)
    {
        state.first_moment.resize(model.parameters());
        state.second_moment.resize(model.parameters());
    }
    for (const auto& layer : model.layers())
    {
        // Glorot uniform initialization with zero biases.
        const auto range = std::sqrt(6.0 / static_cast<double>(layer.input + layer.output));
        for (size_t j = 0; j < layer.output; ++j)
            for (size_t i = 0; i < layer.input; ++i)
                state.weights[layer.neuron(j) + i] = (2.0 * state.random.unit() - 1.0) * range;
    }
    state.consume(model.parameters() * (train.rows.size() + val.rows.size()));
    state.initial_training_loss = dataset_loss(model, state.weights, train, check);
    state.initial_validation_loss = dataset_loss(model, state.weights, val, check);
    state.best_training_loss = state.initial_training_loss;
    state.best_validation_loss = state.initial_validation_loss;
    state.patience_anchor = state.initial_validation_loss;
    state.best_weights = state.weights;
    return state;
}

void train_epoch(const Network& model, const Settings& input, TrainingState& state,
    Partition train, Partition val, std::span<size_t> order, const sql::WorkCheck& check)
{
    std::iota(order.begin(), order.end(), size_t{0});
    for (size_t i = order.size(); i > 1; --i)
        std::swap(order[i - 1], order[state.random.below(i)]);
    for (size_t start = 0; start < order.size(); start += input.batch_size)
    {
        poll(check);
        const auto count = (std::min)(order.size() - start, static_cast<size_t>(input.batch_size));
        state.consume(model.parameters() * (3 * count + 1));
        const auto batch = batch_gradient(model, state.weights, train, order.subspan(start, count), check);
        update_weights(state, input, batch.gradient);
    }
    ++state.epoch;
    state.consume(model.parameters() * (train.rows.size() + val.rows.size()));
    const auto training_loss = dataset_loss(model, state.weights, train, check);
    const auto validation_loss = dataset_loss(model, state.weights, val, check);
    if (validation_loss < state.best_validation_loss)
    {
        state.best_epoch = state.epoch;
        state.best_validation_loss = validation_loss;
        state.best_training_loss = training_loss;
        state.best_weights = state.weights;
    }
    if (state.patience_anchor - validation_loss > input.min_delta)
    {
        state.stale_epochs = 0;
        state.patience_anchor = validation_loss;
    }
    else ++state.stale_epochs;
}

Json make_checkpoint(const Settings& input, const TrainingState& state, size_t train_count, size_t validation_count)
{
    return {{"format", 1}, {"layers", input.layers}, {"settings", save_settings(input)},
        {"rng", {{"engine", "mt19937_64"}, {"seed", state.random.seed()}, {"draws", state.random.draws()}}},
        {"iterator", {{"epoch", state.epoch}, {"position", 0}}},
        {"weights", state.weights}, {"best_weights", state.best_weights},
        {"first_moment", state.first_moment}, {"second_moment", state.second_moment},
        {"optimizer_steps", state.optimizer_steps}, {"best_epoch", state.best_epoch}, {"stale_epochs", state.stale_epochs},
        {"patience_anchor", state.patience_anchor}, {"initial_training_loss", state.initial_training_loss},
        {"initial_validation_loss", state.initial_validation_loss}, {"best_training_loss", state.best_training_loss},
        {"best_validation_loss", state.best_validation_loss}, {"used_operations", state.used_operations},
        {"train_count", train_count}, {"validation_count", validation_count}};
}

TrainingState read_checkpoint(const Json& stored, const Settings& input, const Network& model, uint64_t seed,
    size_t train_count, size_t validation_count, const sql::WorkCheck& check)
{
    shape(stored, {"format", "layers", "settings", "rng", "iterator", "weights", "best_weights", "first_moment",
        "second_moment", "optimizer_steps", "best_epoch", "stale_epochs", "patience_anchor", "initial_training_loss",
        "initial_validation_loss", "best_training_loss", "best_validation_loss", "used_operations",
        "train_count", "validation_count"});
    if (unsigned_number(field(stored, "format")) != 1 || field(stored, "layers") != Json(input.layers) ||
        field(stored, "settings") != save_settings(input) ||
        unsigned_number(field(stored, "train_count")) != train_count ||
        unsigned_number(field(stored, "validation_count")) != validation_count) invalid();
    const auto& cursor = field(stored, "iterator");
    const auto& rng = field(stored, "rng");
    shape(cursor, {"epoch", "position"});
    shape(rng, {"engine", "seed", "draws"});
    const auto epoch = positive(field(cursor, "epoch"), input.epochs);
    const auto count = unsigned_number(field(rng, "draws"));
    if (unsigned_number(field(cursor, "position")) != 0 || text(field(rng, "engine")) != "mt19937_64" ||
        unsigned_number(field(rng, "seed")) != seed ||
        count < model.random_weights() + static_cast<uint64_t>(epoch) * (train_count - 1)) invalid();

    TrainingState state(seed);
    poll(check);
    state.random.restore(count);
    poll(check);
    state.epoch = epoch;
    const auto best = unsigned_number(field(stored, "best_epoch"));
    const auto stale = unsigned_number(field(stored, "stale_epochs"));
    if (best > epoch || stale > epoch || stale > input.patience) invalid();
    state.best_epoch = static_cast<uint32_t>(best);
    state.stale_epochs = static_cast<uint32_t>(stale);
    const auto batches = (train_count + input.batch_size - 1) / input.batch_size;
    state.optimizer_steps = unsigned_number(field(stored, "optimizer_steps"));
    state.used_operations = unsigned_number(field(stored, "used_operations"));
    if (state.optimizer_steps != batches * static_cast<uint64_t>(epoch) ||
        state.used_operations != model.parameters() *
            (train_count + validation_count + static_cast<uint64_t>(epoch) * (4 * train_count + validation_count + batches)) ||
        state.used_operations > maximum_operations) invalid();

    state.patience_anchor = loss_value(field(stored, "patience_anchor"));
    state.initial_training_loss = loss_value(field(stored, "initial_training_loss"));
    state.initial_validation_loss = loss_value(field(stored, "initial_validation_loss"));
    state.best_training_loss = loss_value(field(stored, "best_training_loss"));
    state.best_validation_loss = loss_value(field(stored, "best_validation_loss"));
    if (state.best_validation_loss > state.patience_anchor || state.patience_anchor > state.initial_validation_loss)
        invalid();
    state.weights = read_vector(field(stored, "weights"), model.parameters());
    state.best_weights = read_vector(field(stored, "best_weights"), model.parameters());
    if (!std::ranges::all_of(state.weights, bounded) || !std::ranges::all_of(state.best_weights, bounded)) invalid();
    const auto moments = input.optimizer == Optimizer::adam ? model.parameters() : size_t{0};
    state.first_moment = read_vector(field(stored, "first_moment"), moments);
    state.second_moment = read_vector(field(stored, "second_moment"), moments);
    if (std::ranges::any_of(state.second_moment, [](double value) { return value < 0; })) invalid();
    return state;
}

Json model_artifact(const Settings& input, const TrainingState& state)
{
    return {{"format", 1}, {"layers", input.layers}, {"loss", "bce"},
        {"activation", activations[static_cast<size_t>(input.activation)]},
        {"optimizer", optimizers[static_cast<size_t>(input.optimizer)]}, {"seed", state.random.seed()},
        {"parameters", state.best_weights}, {"optimizer_steps", state.optimizer_steps},
        {"epochs_completed", state.epoch}, {"best_epoch", state.best_epoch},
        {"initial_training_loss", state.initial_training_loss}, {"initial_validation_loss", state.initial_validation_loss},
        {"training_loss", state.best_training_loss}, {"validation_loss", state.best_validation_loss},
        {"early_stopped", state.stale_epochs == input.patience && state.epoch < input.epochs}};
}
}

Json fit_mlp(MlpRows train, std::span<const double> train_y, MlpRows validation,
    std::span<const double> validation_y, const Json& options, uint64_t seed, const sql::WorkCheck& check,
    const Json& resume, const MlpCheckpoint& checkpoint)
{
    if (train.empty() || train.front().empty() || validation.empty()) invalid();
    if (train.size() > maximum_rows || validation.size() > maximum_rows ||
        train.size() + validation.size() > maximum_rows || train.front().size() > maximum_input_width) limit();
    const auto input = settings(options, static_cast<uint32_t>(train.front().size()));
    const Network model(input.layers, input.activation);
    validate_data(train, train_y, input.layers.front(), check, true);
    validate_data(validation, validation_y, input.layers.front(), check);
    const Partition training{train, train_y};
    const Partition valid{validation, validation_y};
    auto state = resume.is_null() ? initial_state(model, input, seed, training, valid, check)
        : read_checkpoint(resume, input, model, seed, train.size(), validation.size(), check);
    std::vector<size_t> order(train.size());
    while (state.epoch < input.epochs && state.stale_epochs < input.patience)
    {
        train_epoch(model, input, state, training, valid, order, check);
        poll(check);
        if (checkpoint) checkpoint(make_checkpoint(input, state, train.size(), validation.size()));
    }
    return model_artifact(input, state);
}

double predict_mlp(const Json& artifact, std::span<const double> row)
{
    if (!artifact.is_object() || unsigned_number(field(artifact, "format")) != 1 ||
        text(field(artifact, "loss")) != "bce") invalid();
    const auto& stored = field(artifact, "layers");
    if (!stored.is_array() || stored.size() < 3 || stored.size() > maximum_hidden_layers + 2) invalid();
    std::vector<uint32_t> widths;
    widths.reserve(stored.size());
    for (const auto& width : stored) widths.push_back(positive(width, maximum_input_width));
    const Network model(widths, find_name<Activation>(text(field(artifact, "activation")), activations));
    const auto values = read_vector(field(artifact, "parameters"), model.parameters());
    if (row.size() != widths.front() || !std::ranges::all_of(row, bounded) ||
        !std::ranges::all_of(values, bounded)) invalid();
    Vector frame(model.values());
    (void)model.forward(values, row, frame);
    return frame.back();
}

MlpGradient mlp_loss_gradient(std::span<const uint32_t> layers, std::string_view activation,
    std::span<const double> parameters, MlpRows rows, std::span<const double> labels)
{
    const Network model(layers, find_name<Activation>(activation, activations));
    validate_data(rows, labels, layers.front(), {});
    if (parameters.size() != model.parameters() || !std::ranges::all_of(parameters, bounded)) invalid();
    std::vector<size_t> order(rows.size());
    std::iota(order.begin(), order.end(), size_t{0});
    return batch_gradient(model, Vector(parameters.begin(), parameters.end()), {rows, labels}, order, {}, true);
}
}
