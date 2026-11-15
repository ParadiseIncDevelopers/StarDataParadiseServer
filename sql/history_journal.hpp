#pragma once

#include "executor.hpp"

namespace sdp::sql
{
inline constexpr uint32_t history_seal_schema = 0x53444831;
inline constexpr uint32_t history_policy_schema = 0x53445031;
inline constexpr uint32_t history_receipt_schema = 0x53445232;

struct HistoryPin
{
    std::string name, kind;
    uint64_t revision;
};

struct HistoryState
{
    uint64_t head{};
    uint64_t floor{1};
    uint64_t quota{32 * 1024 * 1024};
    uint64_t retained_bytes{};
    std::vector<HistoryPin> pins;
};

struct HistoryRevision
{
    uint64_t lsn, parent, transaction;
    std::string actor;
    std::optional<int64_t> utc_seconds;
    std::string sha256;
    std::vector<disk::Change> changes;
};

class HistoryUnavailable : public Error
{
public:
    explicit HistoryUnavailable(uint64_t floor);
    uint64_t floor() const noexcept { return floor_; }

private:
    uint64_t floor_;
};

class HistoryPinned : public Error
{
public:
    explicit HistoryPinned(HistoryPin pin);
    const HistoryPin& pin() const noexcept { return pin_; }
    std::string_view name() const noexcept { return pin_.name; }
    std::string_view kind() const noexcept { return pin_.kind; }
    uint64_t revision() const noexcept { return pin_.revision; }

private:
    HistoryPin pin_;
};

std::string history_sha256(std::span<const disk::Byte> bytes);

class HistoryJournal
{
public:
    using Visitor = std::function<void(HistoryRevision&&)>;

    HistoryJournal(disk::Engine& engine, disk::Transaction& tx, disk::Owner owner, WorkCheck check = {});

    HistoryState state();
    HistoryState verify();
    HistoryRevision read(uint64_t revision);
    // The visitor observes committed revisions under the engine read lock.
    // Zero bounds default to the current visible floor and durable head.
    HistoryState visit(uint64_t first, uint64_t last, const Visitor& visitor);

    void seal(std::string_view actor, int64_t utc_seconds);
    void quota(uint64_t bytes);
    void pin(HistoryPin pin);
    void unpin(std::string_view kind, std::string_view name);
    void purge_before(uint64_t first_retained);

private:
    struct Settings;
    Settings settings();
    void update_settings(const Settings& settings);

    disk::Engine& engine_;
    disk::Transaction& tx_;
    disk::Owner owner_;
    WorkCheck check_;
};
}
