#pragma once

#include <sdp/wire.hpp>

namespace sdp::jobs
{
enum class Misfire { skip, run_once };

Misfire validate_schedule(const wire::Json& rule);
// Returns the next due UTC Unix second strictly after utc_exclusive.
int64_t schedule_after(const wire::Json& rule, int64_t utc_exclusive);
}
