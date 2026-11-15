#pragma once

#include "server.hpp"

namespace sdp::server
{
int windows_service(std::wstring_view operation, const std::filesystem::path& config_path,
    const Config& config);
int run_windows_service(const std::filesystem::path& config_path, std::wstring_view instance);
}

