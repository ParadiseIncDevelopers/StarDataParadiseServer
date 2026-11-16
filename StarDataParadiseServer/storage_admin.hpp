#pragma once

#include "server.hpp"

namespace sdp::server
{
inline constexpr auto store_name = "store.sdp";

void initialize_storage(const Config& config);
void inspect_storage(const Config& config);
void recover_storage(const Config& config);
void checkpoint_storage(const Config& config);
void check_storage_on_start(const Config& config);
void inspect_catalog(const Config& config);
void check_sql(const Config& config, const std::filesystem::path& source);
void check_package_config(const Config& config);
void verify_installation();
}
