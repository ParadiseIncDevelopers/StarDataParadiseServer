#include "storage_admin.hpp"
#include "../storage/transactions.hpp"

#include <iostream>

namespace sdp::server
{
namespace
{
constexpr auto store_name = "store.sdp";

void check_empty(const Config& config)
{
    for (const auto& entry : std::filesystem::directory_iterator(config.data))
    {
        const auto name = entry.path().filename();
        if (name != ".sdp.lock" && name != "admin-v1.json")
            throw StartupError("Storage initialization requires an unused data directory");
    }
}
}

void initialize_storage(const Config& config)
{
    check_empty(config);
    const DataLock lock(config);
    check_empty(config);
    disk::Store::initialize(config.data / store_name);
    std::cout << "Storage initialized\n";
}

void inspect_storage(const Config& config)
{
    const DataLock lock(config);
    const auto view = disk::verify_database(config.data / store_name).integrity;
    std::cout << "Storage OK: " << view.rows << " rows, " << view.heap_pages << " heap pages, "
        << view.overflow_pages << " overflow pages, " << view.free_pages << " free pages\n";
}

void recover_storage(const Config& config)
{
    const DataLock lock(config);
    const auto report = disk::verify_database(config.data / store_name, true);
    std::cout << "Recovery OK at LSN " << report.header.lsn << ", "
        << report.rewritten_pages << " pages rewritten, "
        << (report.discarded_tail ? "uncommitted tail discarded" : "complete WAL") << '\n';
}

void checkpoint_storage(const Config& config)
{
    const DataLock lock(config);
    disk::Engine storage(config.data / store_name);
    const auto result = storage.checkpoint();
    std::cout << "Checkpoint at LSN " << result.published << ", retain from "
        << result.retain_from << ", " << result.reclaimable << " reclaimable segments\n";
}

void check_storage_on_start(const Config& config)
{
    const auto path = config.data / store_name;
    if (std::filesystem::symlink_status(path).type() == std::filesystem::file_type::not_found)
    {
        auto checkpoint = path;
        checkpoint += ".checkpoint";
        if (!disk::segments(path).empty() ||
            std::filesystem::symlink_status(checkpoint).type() != std::filesystem::file_type::not_found)
            throw StartupError("Storage file is missing for retained WAL or checkpoint");
        return;
    }
    disk::verify_database(path, true);
}
}
