#pragma once

#include <sdp/wire.hpp>

namespace sdp::mysql_model
{
wire::Json compute(const wire::Json& request);
wire::Json compute_sql(const wire::Json& request);
}
