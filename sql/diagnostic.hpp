#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

namespace sdp::sql
{
enum class Code { syntax, unsupported, name, type, range, constraint, limit };

struct Source
{
    size_t offset{};
    size_t line{1};
    size_t column{1};
};

class Error : public std::runtime_error
{
public:
    Error(Code code, Source source, std::string message)
        : std::runtime_error(std::move(message)), code_(code), source_(source) {}

    Code code() const noexcept { return code_; }
    Source source() const noexcept { return source_; }

private:
    Code code_;
    Source source_;
};
}
