#pragma once

#include "../storage/transactions.hpp"
#include <sdp/wire.hpp>

#include <atomic>
#include <thread>

namespace sdp::backup { class Archive; }

namespace sdp::jobs
{
struct Reply
{
    wire::Json body;
    bool changed{};
    std::string dataset;
};

class Manager
{
public:
    Manager(disk::Engine& engine, std::filesystem::path temporary, bool enable_mlp = true,
        backup::Archive* archives = nullptr);
    bool backup_enabled() const noexcept { return archives_ != nullptr; }
    wire::Json inspect_backup(std::string_view operation, const wire::Json& body, const std::function<void()>& check);
    bool mlp_enabled() const noexcept { return mlp_enabled_; }
    wire::Json capabilities() const;
    wire::Json statistics(disk::Transaction& tx, disk::Owner owner) const;
    Reply request(disk::Transaction& tx, disk::Owner owner, std::string_view actor, bool admin,
        std::string_view operation, const wire::Json& body, const std::function<void()>& check);
    Reply submit_dataset_job(disk::Transaction& tx, disk::Owner owner, std::string_view actor,
        std::string_view token, std::string_view dataset, wire::Json payload, const std::function<void()>& check,
        int64_t model = 0);
    struct CheckpointData { wire::Json metadata, payload; };
    CheckpointData checkpoint(disk::Transaction& tx, disk::Owner owner, std::string_view actor, bool admin,
        int64_t model, std::string_view id, const std::function<void()>& check) const;
    bool references_dataset(disk::Transaction& tx, disk::Owner owner, std::string_view dataset,
        const std::function<void()>& check) const;
private:
    void run(std::stop_token stop);
    void step(std::stop_token stop);
    void recover(std::stop_token stop);

    struct ProgressState
    {
        std::string id;
        uint32_t pid{};
        uint64_t done{}, total{};
    };
    disk::Engine& engine_;
    std::filesystem::path temporary_;
    const bool mlp_enabled_;
    backup::Archive* const archives_;
    mutable std::mutex progress_mutex_;
    ProgressState progress_;
    std::atomic_bool paused_{false};
    std::jthread runner_;
};
}
