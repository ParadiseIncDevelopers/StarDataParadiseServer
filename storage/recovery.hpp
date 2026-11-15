#pragma once

#include "wal.hpp"

#include <optional>

namespace sdp::disk
{
struct Segment
{
    std::filesystem::path path;
    uint64_t first;
};

struct Recovery
{
    Header header;
    Integrity integrity;
    size_t transactions{};
    size_t rewritten_pages{};
    bool discarded_tail{};
};

struct RetentionPins
{
    std::optional<uint64_t> backup;
    std::optional<uint64_t> dataset;
};

struct Checkpoint
{
    uint64_t published{};
    uint64_t retain_from{1};
    size_t reclaimable{};
};

std::vector<Segment> segments(const std::filesystem::path& data);
Recovery verify_database(const std::filesystem::path& data, bool repair = false,
    std::span<const RowId> index_candidates = {});
void save_checkpoint(const std::filesystem::path& data, Checkpoint value,
    const std::function<void()>& before_publish = {});
Checkpoint retention(uint64_t published, uint64_t history, std::optional<uint64_t> active, const RetentionPins& pins);
}
