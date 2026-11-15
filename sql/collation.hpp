#pragma once

#include <string>
#include <string_view>

namespace sdp::sql
{
std::string_view canonical_collation(std::string_view name);
std::string collation_version(std::string_view name);
int compare_collated(std::string_view left, std::string_view right, std::string_view name);
}
