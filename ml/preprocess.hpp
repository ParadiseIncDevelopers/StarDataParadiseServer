#pragma once

#include <sdp/wire.hpp>

#include <functional>

namespace sdp::ml
{
wire::Json prepare_dataset(const wire::Json& source, const wire::Json& options,
    const std::function<void()>& check = {});
std::vector<double> transform_features(const wire::Json& transforms, const wire::Json& named_features);
}
