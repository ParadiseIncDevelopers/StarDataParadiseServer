#pragma once

#include "storage.hpp"

#include <memory>

namespace sdp::disk
{
enum class ChangeKind : uint32_t { row = 1, catalog = 2 };
struct Change
{
    RowId id{};
    ChangeKind kind{};
    Bytes before;
    Bytes after;
};

struct Frame
{
    Batch batch;
    std::vector<Change> changes;
};

struct Scan
{
    uint64_t first{};
    uint64_t last{};
    uint64_t valid_end{};
    size_t commits{};
    bool incomplete_tail{};
};

constexpr uint64_t wal_quota = 64 * 1024 * 1024;
constexpr size_t change_budget = 2 * 1024 * 1024;
constexpr size_t change_count_limit = 4096;
using VisitFrame = std::function<void(Frame&&)>;

std::filesystem::path segment_path(const std::filesystem::path& data, uint64_t first);
std::unique_ptr<File> open_segment(const std::filesystem::path& path, uint64_t first,
    bool create, IoHook hook = {});
Scan scan(File& file, const VisitFrame& visit = {});
void append_commit(File& file, const Frame& frame, const std::function<void()>& after_body = {},
    uint64_t segment_budget = wal_quota);
}
