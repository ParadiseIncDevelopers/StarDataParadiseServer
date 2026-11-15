#pragma once

#include "catalog.hpp"
#include "../StarDataParadiseServer/server.hpp"

#include <memory>
#include <shared_mutex>

namespace sdp::sql
{
struct Principal
{
    uint64_t id{};
    std::string name;
    bool administrator{};
};

bool security_ddl(const Statement& statement);

class Security
{
public:
    explicit Security(server::Admin bootstrap);

    struct Snapshot;
    std::shared_ptr<const Snapshot> snapshot(disk::Transaction& transaction, disk::Owner owner) const;
    void publish(std::shared_ptr<const Snapshot> snapshot, uint64_t committed_lsn);
    uint64_t commit_and_publish(std::shared_ptr<const Snapshot> snapshot, const std::function<uint64_t()>& commit);

    std::optional<Principal> authenticate(std::string_view user, std::string_view password) const;
    bool allowed(const Principal& actor, std::string_view verb, ObjectId target = 0, ObjectId schema = 0,
        ObjectId database = 0) const;
    void apply(disk::Transaction& transaction, disk::Owner owner, const Principal& actor, const Statement& statement);
    void set_password(disk::Transaction& transaction, disk::Owner owner, const Principal& actor,
        std::string_view user, std::string_view old_password, std::string_view new_password);

private:
    struct Published;
    std::shared_ptr<const Snapshot> current() const;
    server::Admin bootstrap_;
    mutable std::shared_mutex publication_;
    std::shared_ptr<const Published> published_;
    bool blocked_{};
};
}
