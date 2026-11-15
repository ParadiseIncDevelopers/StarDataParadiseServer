#include "support_bundle.hpp"

#include <openssl/rand.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <map>
#include <optional>

namespace sdp::server
{
namespace
{
using wire::Json;
using W = wire::Code;
using Path = std::filesystem::path;
using namespace std::string_view_literals;

constexpr auto categories = std::array{"query"sv, "audit"sv, "recovery"sv, "history"sv,
    "operational"sv, "ml"sv, "decision"sv, "error"sv};
constexpr auto safe_events = std::array{"administrator_created"sv, "authentication_passed"sv, "authentication_failed"sv,
    "listening"sv, "stopped"sv, "session_open"sv, "session_closed"sv, "session_timeout"sv, "session_error"sv,
    "accept_failed"sv, "tls_handshake_failed"sv, "protocol_rejected"sv, "request"sv, "request_completed"sv,
    "request_rejected"sv, "request_failed"sv, "query_prepare"sv, "query_execute"sv, "query_completed"sv,
    "query_failed"sv, "query_cancelled"sv, "transaction_committed"sv, "transaction_rolled_back"sv,
    "recovery_completed"sv, "recovery_failed"sv, "history_verified"sv, "history_applied"sv, "backup_verified"sv,
    "backup_failed"sv, "job_submitted"sv, "job_completed"sv, "job_failed"sv, "model_trained"sv,
    "model_activated"sv, "decision_allowed"sv, "decision_denied"sv, "decision_evaluated"sv,
    "health_checked"sv, "config_changed"sv, "support_created"sv};
constexpr auto forbidden = std::array{"-----begin"sv, "private key"sv, "password="sv, "password:"sv,
    "authorization:"sv, "bearer "sv, "token="sv, "verifier="sv};
constexpr size_t tail_limit = 64 * 1024;
constexpr size_t record_limit = 64;
constexpr size_t line_limit = 2048;

class Handle
{
public:
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    explicit operator bool() const noexcept { return value_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const noexcept { return value_; }

private:
    HANDLE value_;
};

bool plain(const Handle& file, bool directory)
{
    BY_HANDLE_FILE_INFORMATION info{};
    return file && GetFileType(file.get()) == FILE_TYPE_DISK && GetFileInformationByHandle(file.get(), &info) &&
        !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == directory && (directory || info.nNumberOfLinks == 1);
}

Handle directory_handle(const Path& path)
{
    return Handle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
}

bool public_name(std::string_view value, size_t maximum)
{
    return !value.empty() && value.size() <= maximum &&
        value.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") == value.npos;
}

bool hexadecimal_id(std::string_view value)
{
    return value.size() == 32 && value.find_first_not_of("0123456789abcdef") == value.npos;
}

std::optional<std::string> read_tail(const Path& path)
{
    const Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!plain(file, false)) return std::nullopt;
    LARGE_INTEGER bytes{}, start{};
    if (!GetFileSizeEx(file.get(), &bytes) || bytes.QuadPart < 0) return std::nullopt;
    start.QuadPart = (std::max)(0LL, bytes.QuadPart - static_cast<LONGLONG>(tail_limit));
    if (!SetFilePointerEx(file.get(), start, nullptr, FILE_BEGIN)) return std::nullopt;
    std::string content(static_cast<size_t>(bytes.QuadPart - start.QuadPart), '\0');
    DWORD received{};
    if (!content.empty() && !ReadFile(file.get(), content.data(), static_cast<DWORD>(content.size()), &received, nullptr))
        return std::nullopt;
    content.resize(received);
    if (start.QuadPart)
    {
        const auto first = content.find('\n');
        content.erase(0, first == content.npos ? content.size() : first + 1);
    }
    return content;
}

std::optional<uint64_t> number(const Json& row, std::string_view key, uint64_t maximum)
{
    const auto entry = row.find(key);
    if (entry == row.end() || !entry->is_number_unsigned()) return std::nullopt;
    const auto value = entry->get<uint64_t>();
    return value <= maximum ? std::optional(value) : std::nullopt;
}

std::optional<Json> safe_record(std::string_view line, std::map<std::string, size_t>& correlations)
{
    if (line.empty() || line.size() > line_limit) return std::nullopt;
    Json row;
    try { row = wire::parse_payload(line); }
    catch (const wire::Error&) { return std::nullopt; }
    if (!row.is_object()) return std::nullopt;
    const auto at = number(row, "at_utc", 32'503'680'000);
    const auto event = row.find("event");
    if (!at || event == row.end() || !event->is_string()) return std::nullopt;

    const auto& activity = event->get_ref<const std::string&>();
    const std::string_view event_name = activity;
    const bool known = std::ranges::find(safe_events, event_name) != safe_events.end();
    Json sanitized{{"at_utc", *at}, {"event", known ? event_name : "redacted_event"sv}};
    if (const auto code = number(row, "code", static_cast<unsigned>(W::out_of_memory));
        code && (*code == 0 || *code >= static_cast<unsigned>(W::malformed))) sanitized["code"] = *code;
    if (const auto duration = number(row, "duration_ms", 120'000)) sanitized["duration_ms"] = *duration;
    const auto correlation = row.find("correlation_id");
    if (correlation != row.end() && correlation->is_string())
    {
        const auto& value = correlation->get_ref<const std::string&>();
        if (hexadecimal_id(value))
            sanitized["correlation"] = correlations.try_emplace(value, correlations.size() + 1).first->second;
    }
    return sanitized;
}

Json collect_records(std::string_view content, std::map<std::string, size_t>& correlations, size_t& discarded)
{
    auto records = Json::array();
    auto end = content.size();
    while (end && records.size() < record_limit)
    {
        if (content[end - 1] == '\n') --end;
        if (!end) break;
        const auto delimiter = content.rfind('\n', end - 1);
        const auto begin = delimiter == content.npos ? 0 : delimiter + 1;
        auto line = content.substr(begin, end - begin);
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (auto record = safe_record(line, correlations)) records.push_back(std::move(*record));
        else if (!line.empty()) ++discarded;
        end = delimiter == content.npos ? 0 : delimiter;
    }
    std::ranges::reverse(records);
    return records;
}

bool secret_scan(std::string_view document)
{
    std::string lower(document);
    for (auto& value : lower)
        if (value >= 'A' && value <= 'Z') value = static_cast<char>(value + ('a' - 'A'));
    return std::ranges::none_of(forbidden, [&lower](auto needle) { return lower.find(needle) != lower.npos; });
}

std::string staging_name()
{
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) throw wire::Error(W::internal);
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result = ".partial-";
    for (const auto byte : bytes)
    {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

class Staging
{
public:
    explicit Staging(const Path& parent) : path(parent / staging_name())
    {
        if (!CreateDirectoryW(path.c_str(), nullptr)) throw wire::Error(W::invalid_state);
    }
    ~Staging()
    {
        if (!published)
        {
            DeleteFileW((path / L"bundle.json").c_str());
            RemoveDirectoryW(path.c_str());
        }
    }
    Staging(const Staging&) = delete;
    Staging& operator=(const Staging&) = delete;

    void publish(const Path& destination, std::string_view contents)
    {
        {
            const auto file_path = path / L"bundle.json";
            const Handle file(CreateFileW(file_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            DWORD written{};
            if (!plain(file, false) ||
                !WriteFile(file.get(), contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) ||
                written != contents.size() || !FlushFileBuffers(file.get())) throw wire::Error(W::invalid_state);
        }
        if (!MoveFileExW(path.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) throw wire::Error(W::invalid_state);
        published = true;
    }

private:
    Path path;
    bool published{};
};
}

Json support_bundle(const Config& config, std::string_view operation, const Json& body)
{
    const bool create = operation == "admin_support_create";
    if (!create && operation != "admin_support_preview") throw wire::Error(W::unsupported);
    if (!body.is_object() || body.size() != (create ? 1u : 0u)) throw wire::Error(W::invalid_argument);
    const auto target = create ? wire::string_field(body, "target_name") : std::string{};
    if (create && !public_name(target, 32)) throw wire::Error(W::invalid_argument);
    if (!public_name(config.instance_name, 48)) throw wire::Error(W::invalid_state);

    const auto logs = directory_handle(config.logs);
    if (!plain(logs, true)) throw wire::Error(W::invalid_state);
    auto records = Json::object();
    auto counts = Json::object();
    std::map<std::string, size_t> correlations;
    size_t discarded{};
    for (const auto category : categories)
    {
        const auto key = std::string(category);
        const auto content = read_tail(config.logs / (key + ".jsonl"));
        records[key] = content ? collect_records(*content, correlations, discarded) : Json::array();
        counts[key] = records[key].size();
    }

    const Json public_config{{"instance_name", config.instance_name}, {"bind", config.bind}, {"port", config.port},
        {"max_connections", config.max_connections}, {"timeout_ms", config.timeout.count()},
        {"decision_enabled", config.decision_enabled}, {"mlp_cpu_enabled", config.mlp_cpu_enabled},
        {"backup_enabled", !config.backup.empty()}};
    auto summary = Json{{"format", 1}, {"config", public_config}, {"log_counts", std::move(counts)},
        {"discarded_records", discarded}, {"secret_scan_passed", true}};
    auto bundle = summary;
    bundle["logs"] = std::move(records);
    const auto document = bundle.dump();
    if (!secret_scan(document)) throw wire::Error(W::invalid_state);
    if (!create) return summary;

    const auto temporary = directory_handle(config.temp);
    if (!plain(temporary, true)) throw wire::Error(W::invalid_state);
    const auto parent = config.temp / L"support";
    if (!CreateDirectoryW(parent.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        throw wire::Error(W::invalid_state);
    const auto output = directory_handle(parent);
    if (!plain(output, true)) throw wire::Error(W::invalid_state);
    Staging staging(parent);
    staging.publish(parent / target, document);
    summary["target_name"] = target;
    return summary;
}
}
