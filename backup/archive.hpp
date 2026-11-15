#pragma once

#include "../storage/transactions.hpp"
#include <sdp/wire.hpp>

#include <mutex>

namespace sdp::backup
{
class Archive
{
public:
    Archive(disk::Engine& engine, std::filesystem::path root, std::filesystem::path temporary);
    wire::Json create(std::string_view id, uint64_t maximum_bytes, const std::function<void()>& check,
        const std::function<void()>& after_capture);
    wire::Json inspect(const wire::Json& body, std::string_view operation, const std::function<void()>& check);

private:
    disk::Engine& engine_;
    std::filesystem::path root_, temporary_;
    std::mutex mutex_;
};
}
