#include "operations.hpp"

#include "support_bundle.hpp"
#include "../jobs/manager.hpp"
#include "../sql/history_journal.hpp"
#include "../sql/wire_values.hpp"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <openssl/pem.h>
#include <openssl/rand.h>

namespace sdp::server
{
namespace
{
using wire::Json;
using W = wire::Code;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
constexpr disk::Owner owner = UINT64_MAX - 14;
constexpr std::array<std::string_view, 5> checks{"storage", "history", "backup", "certificate", "jobs"};
constexpr std::array<std::string_view, 8> channels{"query", "audit", "recovery", "history", "operational", "ml", "decision", "error"};

int64_t now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

[[noreturn]] void invalid() { throw wire::Error(W::invalid_argument); }
[[noreturn]] void conflict() { throw wire::Error(W::invalid_state); }

Json report(std::string_view status, Json evidence, std::string_view remediation)
{
    return {{"status", status}, {"checked_at", now()}, {"evidence", std::move(evidence)},
        {"remediation", remediation}};
}

std::string identifier(const Config& config)
{
    const auto key = config.instance_name + "\n" + config.data.generic_string();
    return sql::history_sha256({reinterpret_cast<const disk::Byte*>(key.data()), key.size()}).substr(0, 32);
}

std::string random_suffix()
{
    std::array<unsigned char, 12> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) conflict();
    std::string result;
    constexpr std::string_view hex = "0123456789abcdef";
    for (auto byte : bytes)
    {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}

Json config_source(const fs::path& path)
{
    const auto size = fs::file_size(path);
    if (!size || size > 64 * 1024) conflict();
    std::ifstream input(path, std::ios::binary);
    std::string bytes(static_cast<size_t>(size), '\0');
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) conflict();
    const auto result = Json::parse(bytes, nullptr, false);
    if (!result.is_object()) conflict();
    return result;
}

std::string replace_config(const fs::path& path, const Json& config)
{
    const auto suffix = random_suffix();
    auto pending = path;
    pending += ".staged-" + suffix;
    auto previous = path;
    previous += ".previous-" + suffix;
    try
    {
        const auto text = config.dump(2);
        {
            disk::File file(pending, disk::File::Mode::create_new);
            file.write(0, {reinterpret_cast<const disk::Byte*>(text.data()), text.size()});
            file.flush();
        }
        (void)read_config(pending);
        if (!ReplaceFileW(path.c_str(), pending.c_str(), previous.c_str(), 0, nullptr, nullptr))
            conflict();
    }
    catch (...)
    {
        std::error_code ignored;
        fs::remove(pending, ignored);
        throw;
    }
    return previous.filename().string();
}

Json certificate_status(const Config& config)
{
    FILE* input{};
    const auto opened = _wfopen_s(&input, config.certificate.c_str(), L"rb");
    const std::unique_ptr<FILE, decltype(&std::fclose)> file(input, std::fclose);
    if (opened || !file) return report("ERROR", {"certificate_open_failed"}, "Kontrol edilen TLS sertifika dosyasını düzeltin.");
    const std::unique_ptr<X509, decltype(&X509_free)> certificate(
        PEM_read_X509(file.get(), nullptr, nullptr, nullptr), X509_free);
    if (!certificate) return report("ERROR", {"certificate_parse_failed"}, "Geçerli bir TLS sertifikası yükleyin.");
    const auto not_before = X509_cmp_current_time(X509_get0_notBefore(certificate.get()));
    const auto not_after = X509_cmp_current_time(X509_get0_notAfter(certificate.get()));
    if (not_before >= 0 || not_after <= 0)
        return report("ERROR", {"certificate_time_invalid"}, "TLS sertifikasının geçerlilik süresini kontrol edin.");
    int days{}, seconds{};
    if (ASN1_TIME_diff(&days, &seconds, nullptr, X509_get0_notAfter(certificate.get())) != 1)
        return report("ERROR", {"certificate_expiry_unreadable"}, "TLS sertifikasının son geçerlilik tarihini kontrol edin.");
    return report("OK", {{"validity_checked", true}, {"days_until_expiry", days}}, "");
}

bool safe_token(std::string_view token)
{
    return !token.empty() && token.size() <= 40 && token.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == token.npos;
}

struct Rate { int64_t start{}; unsigned count{}; };
std::mutex log_mutex;
std::map<fs::path, Rate> log_rates;
constexpr uintmax_t log_bytes = 2 * 1024 * 1024;
}

Operations::Operations(const Config& config, disk::Engine& engine, jobs::Manager& jobs)
    : config_(config), engine_(engine), jobs_(jobs), instance_id_(identifier(config)),
      timeout_ms_(config.timeout.count()), status_(Json::object())
{
    for (const auto check : checks)
        status_[std::string(check)] = {{"status", "UNKNOWN"}, {"checked_at", nullptr}, {"evidence", nullptr},
            {"remediation", "Bu kontrolü Health Center üzerinden çalıştırın."}};
}

Json Operations::measure(std::string_view check, const std::function<void()>& poll)
{
    try
    {
        if (check == "certificate") return certificate_status(config_);
        if (check == "backup")
        {
            if (!jobs_.backup_enabled())
                return report("UNKNOWN", {"backup_unconfigured"}, "Bağımsız bir yedek dizini yapılandırın.");
            const auto catalog = jobs_.inspect_backup("backup_catalog", Json::object(), poll);
            const auto& backups = catalog.at("backups");
            const Json* latest = nullptr;
            for (const auto& entry : backups)
                if (entry.at("verified") == true && (!latest || entry.at("verified_at") > latest->at("verified_at")))
                    latest = &entry;
            if (!latest)
                return backups.empty() ? report("UNKNOWN", {"no_verified_backup"}, "Bir yedek oluşturup doğrulayın.") :
                    report("ERROR", {"archive_verification_failed"}, "Yedek arşivlerini doğrulayın.");
            return report("OK", *latest, "");
        }

        auto tx = engine_.begin(owner, 5s, {}, 20s, "health-center");
        if (check == "history")
        {
            const auto history = sql::HistoryJournal(engine_, *tx, owner, poll).verify();
            return report("OK", {{"head", history.head}, {"floor", history.floor}, {"retained_bytes", history.retained_bytes},
                {"pin_count", history.pins.size()}}, "");
        }
        if (check == "storage")
        {
            const auto storage = tx->inspect_committed(owner);
            uint64_t models{}, versions{}, bytes{};
            for (const auto& object : sql::Catalog(*tx, owner, poll).list())
            {
                poll();
                if (object.kind != sql::ObjectKind::model) continue;
                ++models;
                const auto metadata = Json::parse(object.definition);
                for (const auto& version : metadata.at("versions"))
                {
                    poll();
                    ++versions;
                    const auto row = tx->get(owner, version.at("artifact_row").get<disk::RowId>());
                    bytes += std::get<std::string>(row.values.at(2)).size();
                }
            }
            return report("OK", {{"lsn", engine_.current().lsn}, {"rows", storage.rows},
                {"heap_pages", storage.heap_pages}, {"overflow_pages", storage.overflow_pages},
                {"free_pages", storage.free_pages}, {"model_count", models}, {"model_versions", versions},
                {"model_artifact_bytes", bytes}}, "");
        }
        auto evidence = jobs_.statistics(*tx, owner);
        return evidence.at("scheduler_paused") == true ?
            report("ERROR", std::move(evidence), "Scheduler’ı ve son iş hatalarını kontrol edin.") :
            report("OK", std::move(evidence), "");
    }
    catch (const wire::Error& error)
    {
        if (error.code() == W::cancelled || error.code() == W::timeout) throw;
        return report("ERROR", {"check_failed"}, "Server loglarını ve yapılandırmayı kontrol edin.");
    }
    catch (const std::exception&) { return report("ERROR", {"check_failed"}, "Server loglarını ve yapılandırmayı kontrol edin."); }
}

Json Operations::configure(const Json& body, bool apply)
{
    if (body.size() != 1) invalid();
    const auto& patch = wire::object_field(body, "config");
    if (patch.empty() || patch.size() > 3) invalid();
    auto dynamic = Json::object();
    auto restart = Json::object();
    for (const auto& field : patch.items())
    {
        const auto& key = field.key();
        if (key == "timeout_ms")
            dynamic[key] = sql::unsigned_field(patch, "timeout_ms", 120000);
        else if (key == "port")
            restart[key] = sql::unsigned_field(patch, "port", 65535);
        else if (key == "max_connections")
            restart[key] = sql::unsigned_field(patch, "max_connections", 128);
        else invalid();
    }
    if ((dynamic.contains("timeout_ms") && dynamic.at("timeout_ms") < 100) ||
        (restart.contains("max_connections") && restart.at("max_connections") == 0)) invalid();
    Json result{{"valid", true}, {"dynamic", dynamic}, {"restart_required", restart}, {"applied", false}};
    if (!apply) return result;

    const std::lock_guard lock(mutex_);
    const auto settings = config_source(config_.source);
    auto updated = settings;
    updated.update(patch);
    if (updated != settings)
        result["previous_config"] = replace_config(config_.source, updated);
    if (dynamic.contains("timeout_ms")) timeout_ms_.store(dynamic.at("timeout_ms").get<int64_t>());
    result["applied"] = true;
    return result;
}

Json Operations::request(std::string_view operation, const Json& body, std::stop_token stop)
{
    try
    {
        if (operation == "admin_health")
        {
            if (!body.empty()) invalid();
            const std::lock_guard lock(mutex_);
            return {{"instance_id", instance_id_}, {"checks", status_}};
        }
        if (operation == "admin_health_check")
        {
            if (body.size() != 1) invalid();
            const auto& check = wire::string_field(body, "check");
            if (std::ranges::find(checks, check) == checks.end()) invalid();
            const auto deadline = std::chrono::steady_clock::now() + 20s;
            auto outcome = measure(check, [&] {
                if (stop.stop_requested()) throw wire::Error(W::cancelled);
                if (std::chrono::steady_clock::now() >= deadline) throw wire::Error(W::timeout);
            });
            const std::lock_guard lock(mutex_);
            status_[check] = outcome;
            return {{"check", check}, {"result", std::move(outcome)}};
        }
        if (operation == "admin_config_preview" || operation == "admin_config_apply")
            return configure(body, operation == "admin_config_apply");
        if (operation == "admin_support_preview" || operation == "admin_support_create")
            return support_bundle(config_, operation, body);
        throw wire::Error(W::unsupported);
    }
    catch (const StartupError&) { conflict(); }
    catch (const fs::filesystem_error&) { conflict(); }
    catch (const disk::Error&) { conflict(); }
    catch (const Json::exception&) { invalid(); }
}

void log_diagnostic(const Config& config, std::string_view channel, std::string_view event,
    uint32_t code, std::string_view correlation, uint64_t duration_ms) noexcept
{
    try
    {
        if (std::ranges::find(channels, channel) == channels.end() || !safe_token(event)) return;
        const auto path = config.logs / (std::string(channel) + ".jsonl");
        const auto at = now();
        const std::lock_guard lock(log_mutex);
        auto& rate = log_rates[path];
        if (at < rate.start || at - rate.start >= 10) rate = {at, 0};
        if (rate.count++ >= 200) return;
        std::error_code error;
        const auto size = fs::file_size(path, error);
        if (!error && size >= log_bytes)
        {
            auto prior = path;
            prior += ".1";
            if (!MoveFileExW(path.c_str(), prior.c_str(), MOVEFILE_REPLACE_EXISTING)) return;
        }

        Json record{{"at_utc", at}, {"event", event}};
        if (code) record["code"] = code;
        if (duration_ms) record["duration_ms"] = duration_ms;
        if (correlation.size() == 32 && correlation.find_first_not_of("0123456789abcdef") == correlation.npos)
            record["correlation_id"] = correlation;
        const auto line = record.dump() + '\n';
        const auto handle = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return;
        FILE_ATTRIBUTE_TAG_INFO file{};
        if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &file, sizeof(file)) &&
            !(file.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
        {
            DWORD written{};
            WriteFile(handle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        }
        CloseHandle(handle);
    }
    catch (...) { }
}
}
