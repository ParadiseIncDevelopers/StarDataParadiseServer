#include "update_snapshot.hpp"
#include "storage_admin.hpp"
#include "../jobs/manager.hpp"
#include "../ml/model.hpp"
#include "../ml/training.hpp"
#include "../sql/history_journal.hpp"
#include "../sql/pl_ir.hpp"
#include "../sql/relational.hpp"
#include "../sql/routines.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <memory>

namespace sdp::server
{
namespace
{
namespace fs = std::filesystem;
using wire::Json;
using namespace std::chrono_literals;
constexpr disk::Owner owner = 1;
constexpr uint64_t max_bytes = uint64_t{disk::page_limit} * disk::page_size + disk::wal_quota + disk::page_size + 4096;
constexpr size_t max_members = 4096;
constexpr std::string_view admin_file = "admin-v1.json";

std::string hex(std::span<const unsigned char> data)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    for (const auto byte : data)
    {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

bool within(const fs::path& directory, const fs::path& path)
{
    const auto equal = [](const fs::path& a, const fs::path& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
    };
    return std::ranges::mismatch(directory, path, equal).in1 == directory.end();
}

fs::path checked_parent(const fs::path& path)
{
    if (!path.is_absolute()) throw StartupError("An absolute update path is required");
    auto current = path.root_path();
    for (const auto& component : path.relative_path())
    {
        if (component == L"." || component == L"..") throw StartupError("Invalid update path component");
        current /= component;
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw StartupError("The update parent must be an existing directory without reparse points");
    }
    return fs::canonical(path);
}

fs::path checked_destination(const Config& config, const fs::path& requested)
{
    if (!requested.is_absolute() || !requested.has_filename() || requested.filename() == L"." ||
        requested.filename() == L"..")
        throw StartupError("An absolute update destination with a fresh directory name is required");
    const auto target = checked_parent(requested.parent_path()) / requested.filename();
    for (const auto& source : {config.data, config.logs, config.temp, config.backup})
        if (!source.empty() && within(fs::canonical(source), target))
            throw StartupError("The update destination overlaps an existing instance directory");
    if (fs::symlink_status(target).type() != fs::file_type::not_found)
        throw StartupError("The update destination already exists");
    return target;
}

class Staging
{
public:
    explicit Staging(const fs::path& parent) : parent_(parent)
    {
        std::array<unsigned char, 16> entropy{};
        if (RAND_bytes(entropy.data(), static_cast<int>(entropy.size())) != 1)
            throw StartupError("Cannot create a private update workspace");
        path_ = parent / (".sdp-update-" + hex(entropy));
        if (!fs::create_directory(path_)) throw StartupError("Update workspace already exists");
    }

    ~Staging()
    {
        if (path_.empty()) return;
        std::error_code error;
        const auto resolved = fs::canonical(path_, error);
        if (!error && resolved == path_ && resolved.parent_path() == parent_)
            fs::remove_all(resolved, error);
    }

    const fs::path& path() const noexcept { return path_; }

    void publish(const fs::path& destination)
    {
        if (!MoveFileExW(path_.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
            throw StartupError("Could not publish the update snapshot into an unused destination");
        path_.clear();
    }

private:
    fs::path parent_, path_;
};

struct Member
{
    std::string name;
    uint64_t bytes;
    std::unique_ptr<disk::File> file;
};

struct Snapshot
{
    std::vector<Member> members;
    uint64_t bytes{};
};

Snapshot open_snapshot(const fs::path& directory)
{
    const auto store = directory / store_name;
    const auto segments = disk::segments(store);
    if (segments.size() > max_members - 3)
        throw StartupError("The retained WAL file count exceeds the update limit");

    Snapshot result;
    const auto add = [&](const fs::path& path, uint64_t member_limit) {
        auto input = std::make_unique<disk::File>(path, disk::File::Mode::read_only);
        const auto bytes = input->size();
        if (!bytes || bytes > member_limit || bytes > max_bytes - result.bytes)
            throw StartupError("The update snapshot has an invalid file size");
        result.bytes += bytes;
        result.members.push_back({path.filename().string(), bytes, std::move(input)});
    };

    add(store, uint64_t{disk::page_limit} * disk::page_size);
    add(directory / admin_file, 4096);
    const auto checkpoint = directory / "store.sdp.checkpoint";
    if (fs::symlink_status(checkpoint).type() != fs::file_type::not_found) add(checkpoint, disk::page_size);
    uint64_t wal_limit = disk::wal_quota;
    for (const auto& segment : segments)
    {
        add(segment.path, wal_limit);
        wal_limit -= result.members.back().bytes;
    }
    std::ranges::sort(result.members, {}, &Member::name);
    return result;
}

std::string snapshot_digest(const Snapshot& snapshot, const fs::path* destination = nullptr)
{
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
        throw StartupError("Cannot initialize the update digest");
    const auto digest = [&](const void* buffer, size_t length) {
        if (EVP_DigestUpdate(context.get(), buffer, length) != 1)
            throw StartupError("Cannot update the snapshot digest");
    };
    constexpr std::string_view domain = "SDP update snapshot format 1\n";
    digest(domain.data(), domain.size());
    std::array<disk::Byte, 64 * 1024> buffer;
    for (const auto& member : snapshot.members)
    {
        const auto framing = member.name + "\n" + std::to_string(member.bytes) + "\n";
        digest(framing.data(), framing.size());
        std::unique_ptr<disk::File> output;
        if (destination)
            output = std::make_unique<disk::File>(*destination / member.name, disk::File::Mode::create_new);
        for (uint64_t offset = 0; offset < member.bytes;)
        {
            const auto count = static_cast<size_t>(std::min(uint64_t{buffer.size()}, member.bytes - offset));
            auto bytes = std::span(buffer).first(count);
            member.file->read(offset, bytes);
            digest(bytes.data(), count);
            if (output) output->write(offset, bytes);
            offset += count;
        }
        if (output) output->flush();
    }
    std::array<unsigned char, 32> result{};
    unsigned length{};
    if (EVP_DigestFinal_ex(context.get(), result.data(), &length) != 1 || length != result.size())
        throw StartupError("Cannot finalize the update digest");
    return hex(result);
}

Json validate_snapshot(const Config& config, bool publish)
{
    const auto storage_path = config.data / store_name;
    const auto baseline = disk::verify_database(storage_path).header;
    static_cast<void>(load_admin(config));
    {
        disk::Store store(storage_path, true);
        sql::verify_relational(store);
    }

    disk::Engine engine(storage_path, {}, config.decision_enabled);
    auto transaction = engine.begin(owner, 0ms, {}, 30s, "offline-update-check");
    sql::Catalog catalog(*transaction, owner);
    const auto objects = catalog.list();
    if (!config.decision_enabled && std::ranges::find(objects, sql::ObjectKind::decision, &sql::Object::kind) != objects.end())
        throw StartupError("Active Decision rules require the Decision module");
    const auto routines = sql::verify_routines(catalog);
    const auto history = sql::HistoryJournal(engine, *transaction, owner).verify();
    const auto jobs = jobs::update_job_state(*transaction, owner);
    const auto& states = jobs.at("states");
    for (const auto key : {"queued", "starting", "running", "checkpointing"})
        if (states.at(key) != 0)
            throw StartupError("Update requires all active jobs to finish, pause, or cancel before stopping the Server");

    size_t model_versions{};
    for (const auto& object : objects)
    {
        if (object.kind != sql::ObjectKind::model) continue;
        const auto model = ml::require_model(catalog, sql::Catalog::qualified(object, objects), {0, "", true});
        for (const auto& version : model.metadata.at("versions"))
        {
            static_cast<void>(ml::load_model(*transaction, owner, model, version));
            ++model_versions;
        }
    }
    transaction->rollback(owner);
    transaction.reset();
    if (engine.current().lsn != baseline.lsn)
        throw StartupError("Update inspection changed the committed revision");

    Json report{{"format", 1}, {"product_version", product_version}, {"lsn", std::to_string(baseline.lsn)},
        {"history_head", std::to_string(history.head)}, {"routines", routines}, {"model_versions", model_versions},
        {"job_states", jobs}, {"decision_enabled", config.decision_enabled}, {"ml_cpu_enabled", config.ml_cpu_enabled},
        {"mlp_cpu_enabled", config.mlp_cpu_enabled && config.ml_cpu_enabled && ml::mlp_compiled()},
        {"format_versions", {{"protocol", wire::version}, {"storage", 1}, {"wal", 1}, {"checkpoint", 1},
            {"history", 1}, {"plsql_ir", pl::ir_version}, {"model", 1}}}};
    if (publish) report["checkpoint_lsn"] = std::to_string(engine.checkpoint().published);
    return report;
}
}

Json update_snapshot(const Config& config, const fs::path& destination, bool readiness)
{
    const DataLock lock(config);
    const auto target = destination.empty() ? fs::path{} : checked_destination(config, destination);
    const auto parent = target.empty() ? checked_parent(config.temp) : target.parent_path();
    const auto original = open_snapshot(config.data);
    if (fs::space(parent).available < original.bytes + 2 * disk::page_size)
        throw StartupError("Insufficient free space for the update snapshot");
    Staging staging(parent);
    const auto source_digest = snapshot_digest(original, &staging.path());
    auto copy = config;
    copy.data = staging.path();
    auto report = validate_snapshot(copy, !target.empty());
    if (readiness)
    {
        report["port"] = probe_update_listener(copy);
        report["bind"] = config.bind;
        report["readiness"] = true;
    }
    report["source_sha256"] = source_digest;
    report["source_bytes"] = original.bytes;
    if (target.empty())
    {
        report["sha256"] = source_digest;
        report["bytes"] = original.bytes;
    }
    else
    {
        const auto verified = open_snapshot(copy.data);
        report["sha256"] = snapshot_digest(verified);
        report["bytes"] = verified.bytes;
    }
    if (!target.empty()) staging.publish(target);
    return report;
}
}
