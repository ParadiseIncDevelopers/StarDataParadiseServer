#pragma once

#include "server.hpp"

namespace sdp::server
{
void initialize_storage(const Config& config);
void inspect_storage(const Config& config);
void recover_storage(const Config& config);
void checkpoint_storage(const Config& config);
void check_storage_on_start(const Config& config);
}
