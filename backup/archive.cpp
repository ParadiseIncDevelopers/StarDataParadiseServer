#include "archive.hpp"
#include "../sql/binder.hpp"
#include "../ml/model.hpp"
#include "../sql/history_journal.hpp"
#include "../sql/wire_values.hpp"
#include "../StarDataParadiseServer/storage_admin.hpp"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <memory>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <regex>
#include <set>

namespace sdp::backup
{
namespace
{
using wire::Json;
using W = wire::Code;
namespace fs = std::filesystem;
using Check = std::function<void()>;
using namespace std::chrono_literals;
constexpr disk::Owner owner = UINT64_MAX - 12;
constexpr size_t maximum_catalog = 16;
constexpr uint64_t maximum_manifest = 256 * 1024;
constexpr uint64_t maximum_logical = 32 * 1024 * 1024;
constexpr uint64_t maximum_archive = 512 * 1024 * 1024;
constexpr auto storage_name = server::store_name;

[[noreturn]] void invalid() { throw wire::Error(W::invalid_argument); }
[[noreturn]] void bad() { throw wire::Error(W::invalid_state); }
void poll(const Check& check) { if (check) check(); }

bool plain(const fs::path& path, bool directory)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        bool(attributes & FILE_ATTRIBUTE_DIRECTORY) == directory;
}

std::string hex(std::span<const unsigned char> input)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(input.size() * 2);
    for (auto value : input)
    {
        result.push_back(digits[value >> 4]);
        result.push_back(digits[value & 15]);
    }
    return result;
}

bool identity(std::string_view id)
{
    return id.size() == 32 && id.find_first_not_of("0123456789abcdef") == id.npos;
}

bool target_name(std::string_view id)
{
    return !id.empty() && id.size() <= 32 && id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") == id.npos;
}

bool archive_member(const std::string& name)
{
    static const std::regex pattern(R"(^store[.]sdp([.]checkpoint|[.]wal([.][0-9]{20})?)?$)");
    return name == "logical.json" || std::regex_match(name, pattern);
}

class Temporary
{
public:
    Temporary(const fs::path& parent, std::string_view prefix)
    {
        std::array<unsigned char, 12> id{};
        if (RAND_bytes(id.data(), static_cast<int>(id.size())) != 1) bad();
        path = parent / (std::string(prefix) + hex(id));
        if (!fs::create_directory(path) || !plain(path, true)) bad();
    }
    ~Temporary()
    {
        if (active)
        {
            std::error_code error;
            fs::remove_all(path, error);
        }
    }
    void publish(const fs::path& destination)
    {
        if (!MoveFileExW(path.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) bad();
        active = false;
    }
    Temporary(const Temporary&) = delete;
    Temporary& operator=(const Temporary&) = delete;
    fs::path path;
private:
    bool active = true;
};

Json read_json(const fs::path& path, uint64_t maximum)
{
    if (!plain(path, false)) bad();
    const auto size = fs::file_size(path);
    if (!size || size > maximum) bad();
    std::ifstream file(path, std::ios::binary);
    std::string bytes(static_cast<size_t>(size), '\0');
    if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) || file.peek() != EOF) bad();
    auto value = Json::parse(bytes, nullptr, false);
    if (!value.is_object()) bad();
    return value;
}

void write_json(const fs::path& path, const Json& value, uint64_t maximum)
{
    const auto bytes = value.dump();
    if (bytes.size() > maximum) throw wire::Error(W::resource_limit);
    disk::File output(path, disk::File::Mode::create_new);
    output.write(0, {reinterpret_cast<const disk::Byte*>(bytes.data()), bytes.size()});
    output.flush();
}

std::string hash_file(const fs::path& path, uint64_t expected_size, const Check& check)
{
    if (!plain(path, false) || fs::file_size(path) != expected_size || expected_size > maximum_archive) bad();
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!hash || EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) != 1) bad();
    std::ifstream file(path, std::ios::binary);
    std::array<char, 65536> bytes{};
    auto remaining = expected_size;
    while (remaining)
    {
        poll(check);
        const auto count = static_cast<std::streamsize>((std::min)(remaining, uint64_t{bytes.size()}));
        if (!file.read(bytes.data(), count) || EVP_DigestUpdate(hash.get(), bytes.data(), static_cast<size_t>(count)) != 1) bad();
        remaining -= static_cast<uint64_t>(count);
    }
    if (!file || file.peek() != EOF) bad();
    std::array<unsigned char, 32> result{};
    unsigned size{};
    if (EVP_DigestFinal_ex(hash.get(), result.data(), &size) != 1 || size != result.size()) bad();
    return hex(result);
}

Json history_rule(const sql::HistoryState& state)
{
    auto pins = Json::array();
    for (const auto& pin : state.pins)
        pins.push_back({{"kind", pin.kind}, {"name", pin.name}, {"revision", pin.revision}});
    return {{"head", state.head}, {"floor", state.floor}, {"retained_bytes", state.retained_bytes},
        {"pins", std::move(pins)}};
}

Json logical(disk::Engine& engine, disk::Transaction& tx, const Check& check)
{
    const auto history = sql::HistoryJournal(engine, tx, owner, check).verify();
    sql::Catalog catalog(tx, owner, check);
    auto definitions = Json::array();
    auto objects = catalog.list();
    std::ranges::sort(objects, {}, &sql::Object::id);
    size_t budget{};
    for (const auto& object : objects)
    {
        poll(check);
        auto columns = Json::array();
        for (const auto& column : object.columns)
            columns.push_back({{"name", column.name}, {"type", sql::type_name(column.type)},
                {"nullable", column.nullable}, {"ordinal", column.ordinal}});
        Json item{{"id", object.id}, {"parent", object.parent}, {"kind", static_cast<int>(object.kind)},
            {"name", object.name}, {"version", object.version}, {"definition", object.definition},
            {"columns", std::move(columns)}};
        budget += item.dump().size();
        if (object.kind == sql::ObjectKind::table)
        {
            auto rows = Json::array();
            sql::scan_table(tx, owner, object, [&](disk::RowId row_id, const sql::RowValues& values) {
                auto row = Json::array();
                for (size_t index = 0; index < values.size(); ++index)
                    row.push_back(sql::write_value(values[index], object.columns.at(index).type));
                Json record{{"id", row_id}, {"values", std::move(row)}};
                budget += record.dump().size() + 1;
                if (budget > maximum_logical) throw wire::Error(W::resource_limit);
                rows.push_back(std::move(record));
                return true;
            }, check);
            item["rows"] = std::move(rows);
        }
        if (object.kind == sql::ObjectKind::model)
        {
            const ml::Model model{object, Json::parse(object.definition)};
            for (const auto& version : model.metadata.at("versions"))
                (void)ml::load_model(tx, owner, model, version);
        }
        definitions.push_back(std::move(item));
        if (budget > maximum_logical) throw wire::Error(W::resource_limit);
    }
    return {{"format", 1}, {"lsn", engine.current().lsn}, {"objects", std::move(definitions)},
        {"history", history_rule(history)}};
}

std::vector<fs::path> ready_archives(const fs::path& root)
{
    if (!plain(root, true)) bad();
    std::vector<fs::path> ready;
    for (const auto& entry : fs::directory_iterator(root))
    {
        if (!identity(entry.path().filename().string())) continue;
        if (!plain(entry.path(), true)) bad();
        ready.push_back(entry.path());
        if (ready.size() > maximum_catalog) throw wire::Error(W::resource_limit);
    }
    std::ranges::sort(ready);
    return ready;
}

Json manifest(const fs::path& source, std::string_view id, const Check& check)
{
    if (!plain(source, true) || !identity(id)) bad();
    const auto value = read_json(source / "manifest.json", maximum_manifest);
    if (value.value("format", 0) != 1 || value.at("verified") != true ||
        value.at("backup_id").get_ref<const std::string&>() != id || !value.at("lsn").is_number_unsigned() ||
        !value.at("files").is_object() || value.at("files").size() < 3 || value.at("files").size() > 128) bad();
    std::set<std::string> names;
    uint64_t total{};
    for (const auto& entry : fs::directory_iterator(source))
        names.insert(entry.path().filename().string());
    if (names.size() != value.at("files").size() + 1 || names.erase("manifest.json") != 1) bad();
    for (const auto& file : value.at("files").items())
    {
        const auto size = file.value().at("bytes").get<uint64_t>();
        if (!archive_member(file.key()) || names.erase(file.key()) != 1 || size > maximum_archive - total ||
            hash_file(source / file.key(), size, check) != file.value().at("sha256").get_ref<const std::string&>())
            bad();
        total += size;
    }
    if (!value.at("files").contains(storage_name) || !value.at("files").contains("logical.json")) bad();
    return value;
}

void restore_contents(const fs::path& source, const Json& proof, const fs::path& target, const Check& check)
{
    uint64_t required{};
    for (const auto& file : proof.at("files").items())
        if (file.key() != "logical.json") required += file.value().at("bytes").get<uint64_t>();
    if (fs::space(target).available < required) throw wire::Error(W::resource_limit);
    for (const auto& file : proof.at("files").items())
    {
        poll(check);
        if (file.key() == "logical.json") continue;
        const auto destination = target / file.key();
        if (!fs::copy_file(source / file.key(), destination, fs::copy_options::none) ||
            hash_file(destination, file.value().at("bytes").get<uint64_t>(), check) != file.value().at("sha256").get_ref<const std::string&>())
            bad();
    }
    const auto restored = disk::verify_database(target / storage_name);
    if (restored.header.lsn != proof.at("lsn").get<uint64_t>()) bad();
    disk::Engine opened(target / storage_name);
    auto tx = opened.begin(owner, 0ms, {}, 30s, "backup-verify");
    if (read_json(source / "logical.json", maximum_logical) != logical(opened, *tx, check)) bad();
}

Json summary(const fs::path& source, const Json& proof, const Check& check)
{
    return {{"backup_id", proof.at("backup_id")}, {"lsn", proof.at("lsn")}, {"verified", true},
        {"verified_at", proof.at("verified_at")},
        {"manifest_sha256", hash_file(source / "manifest.json", fs::file_size(source / "manifest.json"), check)}};
}
}

Archive::Archive(disk::Engine& engine, fs::path root, fs::path temporary)
    : engine_(engine), root_(std::move(root)), temporary_(std::move(temporary))
{
    if (!plain(root_, true) || !plain(temporary_, true)) bad();
}

Json Archive::create(std::string_view id, uint64_t maximum_bytes, const Check& check,
    const Check& after_capture)
{
    if (!identity(id) || !maximum_bytes || maximum_bytes > maximum_archive) invalid();
    const std::lock_guard lock(mutex_);
    const auto destination = root_ / std::string(id);
    if (plain(destination, true)) return summary(destination, manifest(destination, id, check), check);
    if (ready_archives(root_).size() == maximum_catalog) throw wire::Error(W::resource_limit);
    Temporary capture(root_, ".partial-");
    Json logical_dump;
    disk::PhysicalSnapshot captured;
    {
        auto tx = engine_.begin(owner, 5s, {}, 30s, "backup");
        logical_dump = logical(engine_, *tx, check);
        captured = tx->copy_physical_snapshot(owner, capture.path / storage_name, maximum_bytes, check);
    }
    poll(after_capture);
    if (logical_dump.at("lsn") != captured.header.lsn) bad();
    const auto logical_path = capture.path / "logical.json";
    write_json(logical_path, logical_dump, maximum_logical);
    captured.files.push_back(logical_path);
    const auto logical_bytes = fs::file_size(logical_path);
    if (logical_bytes > maximum_bytes || captured.bytes > maximum_bytes - logical_bytes)
        throw wire::Error(W::resource_limit);
    auto files = Json::object();
    for (const auto& path : captured.files)
    {
        poll(after_capture);
        const auto size = fs::file_size(path);
        files[path.filename().string()] = {{"bytes", size}, {"sha256", hash_file(path, size, after_capture)}};
    }
    Json proof{{"format", 1}, {"backup_id", id}, {"lsn", captured.header.lsn},
        {"checkpoint", {{"published", captured.checkpoint.published}, {"retain_from", captured.checkpoint.retain_from}}},
        {"files", std::move(files)}, {"verified", true},
        {"verified_at", std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()}};
    {
        Temporary verify(temporary_, ".verify-");
        restore_contents(capture.path, proof, verify.path, after_capture);
    }
    if (proof.dump().size() > maximum_bytes - logical_bytes - captured.bytes)
        throw wire::Error(W::resource_limit);
    write_json(capture.path / "manifest.json", proof, maximum_manifest);
    const auto answer = summary(capture.path, proof, after_capture);
    poll(after_capture);
    capture.publish(destination);
    return answer;
}

Json Archive::inspect(const Json& body, std::string_view operation, const Check& check)
{
    const std::lock_guard lock(mutex_);
    try
    {
        if (operation == "backup_catalog")
        {
            if (!body.empty()) invalid();
            auto entries = Json::array();
            for (const auto& source : ready_archives(root_))
            {
                poll(check);
                const auto id = source.filename().string();
                try { entries.push_back(summary(source, manifest(source, id, check), check)); }
                catch (const wire::Error& error)
                {
                    if (error.code() != W::invalid_state) throw;
                    entries.push_back({{"backup_id", id}, {"verified", false}, {"reason", "archive_invalid"}});
                }
            }
            return {{"backups", std::move(entries)}, {"pitr", false}};
        }
        const bool restore = operation == "backup_restore";
        if (body.size() != (restore ? 2 : 1)) invalid();
        const auto& id = wire::string_field(body, "backup_id");
        if (!identity(id)) invalid();
        const auto source = root_ / id;
        if (operation == "backup_delete")
        {
            if (!plain(source, true)) bad();
            const auto tombstone = root_ / (".deleted-" + id);
            if (!MoveFileExW(source.c_str(), tombstone.c_str(), MOVEFILE_WRITE_THROUGH)) bad();
            std::error_code error;
            fs::remove_all(tombstone, error);
            return {{"backup_id", id}, {"deleted", true}, {"cleanup_pending", bool(error)}};
        }
        if (!restore && operation != "backup_verify") throw wire::Error(W::unsupported);
        const auto proof = manifest(source, id, check);
        auto answer = summary(source, proof, check);
        fs::path parent = temporary_;
        fs::path destination;
        if (restore)
        {
            const auto& name = wire::string_field(body, "target_name");
            if (!target_name(name)) invalid();
            parent /= "restores";
            fs::create_directory(parent);
            if (!plain(parent, true)) bad();
            destination = parent / name;
            if (fs::exists(fs::symlink_status(destination))) bad();
        }
        Temporary stage(parent, ".restore-");
        restore_contents(source, proof, stage.path, check);
        if (restore)
        {
            stage.publish(destination);
            answer["target_name"] = wire::string_field(body, "target_name");
            answer["restored"] = true;
        }
        return answer;
    }
    catch (const fs::filesystem_error&) { bad(); }
    catch (const disk::Error&) { bad(); }
    catch (const Json::exception&) { bad(); }
}
}
