#include "server.hpp"

#include <asio/ip/address.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>

namespace sdp::server
{
namespace
{
using wire::Json;
constexpr uint64_t iterations = 600000;
constexpr size_t max_password = 1024;
constexpr size_t max_config = 64 * 1024;
constexpr uintmax_t log_limit = 256 * 1024;
constexpr auto admin_file = "admin-v1.json";

struct CloseFile
{
    void operator()(void* file) const noexcept { CloseHandle(file); }
};
using File = std::unique_ptr<void, CloseFile>;

struct LocalDeleter
{
    void operator()(void* data) const noexcept { LocalFree(data); }
};

struct Secret
{
    std::array<char, max_password + 1> bytes{};
    size_t size{};
    ~Secret() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
    std::string_view view() const { return {bytes.data(), size}; }
};

std::string read_text(const std::filesystem::path& path, size_t limit, const char* problem)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw StartupError(problem);
    std::string buffer(limit + 1, '\0');
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    if (!file.eof() || file.bad() || static_cast<size_t>(file.gcount()) > limit) throw StartupError(problem);
    buffer.resize(static_cast<size_t>(file.gcount()));
    return buffer;
}

std::filesystem::path configured_path(const Json& config, const std::filesystem::path& root,
    const char* key, bool directory)
{
    std::error_code error;
    const auto& value = wire::string_field(config, key);
    if (value.empty() || value.find('\0') != value.npos) throw StartupError("Invalid path in configuration");
    std::u8string utf8;
    utf8.reserve(value.size());
    for (const auto byte : value) utf8.push_back(static_cast<char8_t>(byte));
    auto path = std::filesystem::canonical(root / std::filesystem::path(utf8), error);
    const bool valid = directory ? std::filesystem::is_directory(path, error)
                                 : std::filesystem::is_regular_file(path, error);
    if (error || !valid) throw StartupError("A configured directory or TLS file is missing");
    return path;
}

unsigned number(const Json& config, const char* key, unsigned fallback, unsigned low, unsigned high)
{
    const auto value = config.find(key);
    if (value == config.end()) return fallback;
    if (!value->is_number_unsigned() || *value < low || *value > high) throw StartupError("Invalid numeric configuration value");
    return value->get<unsigned>();
}

bool username_valid(std::string_view name)
{
    return !name.empty() && name.size() <= 64
        && std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        });
}

std::string hex(std::span<const unsigned char> bytes)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string output;
    output.reserve(bytes.size() * 2);
    for (const auto byte : bytes)
    {
        output += digits[byte >> 4];
        output += digits[byte & 15];
    }
    return output;
}

void decode_hex(std::string_view text, std::span<unsigned char> output)
{
    constexpr std::string_view digits = "0123456789abcdef";
    if (text.size() != output.size() * 2) throw StartupError("Invalid administrator verifier");
    for (size_t i = 0; i < output.size(); ++i)
    {
        const auto a = digits.find(text[i * 2]);
        const auto b = digits.find(text[i * 2 + 1]);
        if (a == digits.npos || b == digits.npos) throw StartupError("Invalid administrator verifier");
        output[i] = static_cast<unsigned char>((a << 4) | b);
    }
}

void derive(std::string_view password, const Admin& admin, std::span<unsigned char, 32> output)
{
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), admin.salt.data(),
        static_cast<int>(admin.salt.size()), static_cast<int>(iterations), EVP_sha256(),
        static_cast<int>(output.size()), output.data()) != 1)
        throw StartupError("Password derivation failed");
}

class ConsoleInput
{
public:
    ConsoleInput() : console_(GetStdHandle(STD_INPUT_HANDLE)), interactive_(GetConsoleMode(console_, &mode_) != 0)
    {
        if (interactive_ && !SetConsoleMode(console_, mode_ & ~ENABLE_ECHO_INPUT))
            throw StartupError("Unable to hide console input");
    }

    ~ConsoleInput()
    {
        if (interactive_) SetConsoleMode(console_, mode_);
    }

    void read(Secret& password, const char* prompt)
    {
        if (interactive_)
        {
            std::cerr << prompt << std::flush;
            std::array<wchar_t, max_password + 3> input{};
            DWORD size{};
            const bool got_line = ReadConsoleW(console_, input.data(), static_cast<DWORD>(input.size()), &size, nullptr) != 0
                && size > 0 && input[size - 1] == L'\n';
            while (size && (input[size - 1] == L'\n' || input[size - 1] == L'\r')) --size;
            const int count = got_line ? WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                static_cast<int>(size), password.bytes.data(), static_cast<int>(max_password), nullptr, nullptr) : 0;
            OPENSSL_cleanse(input.data(), input.size() * sizeof(wchar_t));
            std::cerr << '\n';
            if (count <= 0) throw StartupError("Unable to read a valid password");
            password.size = static_cast<size_t>(count);
            return;
        }

        int character;
        while ((character = std::cin.get()) != '\n' && character != EOF)
        {
            if (password.size == password.bytes.size()) throw StartupError("Password exceeds the byte limit");
            password.bytes[password.size++] = static_cast<char>(character);
        }
        if (password.size && password.bytes[password.size - 1] == '\r') --password.size;
        if (password.size > max_password) throw StartupError("Password exceeds the byte limit");
    }

private:
    HANDLE console_;
    DWORD mode_{};
    bool interactive_;
};

void write_admin(const Config& config, const Admin& admin)
{
    const auto destination = config.data / admin_file;
    std::array<unsigned char, 16> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) throw StartupError("System random source failed");
    const auto temporary = config.data / ("admin-" + hex(random) + ".tmp");
    PSECURITY_DESCRIPTOR descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr))
        throw StartupError("Administrator verifier permissions unavailable");

    std::unique_ptr<void, LocalDeleter> security(descriptor);
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    const auto raw = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw == INVALID_HANDLE_VALUE) throw StartupError("Cannot create administrator verifier");
    File file(raw);
    const auto data = Json{{"username", admin.username}, {"algorithm", "pbkdf2-sha256"},
        {"iterations", iterations}, {"salt", hex(admin.salt)}, {"verifier", hex(admin.verifier)}}.dump();
    DWORD written{};
    const bool complete = WriteFile(file.get(), data.data(), static_cast<DWORD>(data.size()), &written, nullptr)
        && written == data.size() && FlushFileBuffers(file.get());
    file.reset();
    if (!complete || !MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporary.c_str());
        throw StartupError("Cannot publish administrator verifier");
    }
}
}

Config read_config(const std::filesystem::path& path)
{
    try
    {
        auto root = std::filesystem::absolute(path).parent_path();
        const auto json = wire::parse_payload(read_text(path, max_config, "Cannot read configuration"));
        constexpr std::array keys{"data_dir", "log_dir", "temp_dir", "tls_certificate", "tls_private_key",
            "bind", "allow_remote", "port", "max_connections", "timeout_ms", "routine_test_instance"};
        for (const auto& field : json.items())
            if (std::find(keys.begin(), keys.end(), field.key()) == keys.end())
                throw StartupError("Unrecognized configuration key");

        Config result;
        result.data = configured_path(json, root, "data_dir", true);
        result.logs = configured_path(json, root, "log_dir", true);
        result.temp = configured_path(json, root, "temp_dir", true);
        result.certificate = configured_path(json, root, "tls_certificate", false);
        result.private_key = configured_path(json, root, "tls_private_key", false);
        if (std::filesystem::equivalent(result.data, result.logs) ||
            std::filesystem::equivalent(result.data, result.temp) ||
            std::filesystem::equivalent(result.logs, result.temp))
            throw StartupError("Data, log and temp directories must differ");

        result.routine_test_instance = json.value("routine_test_instance", false);
        if (json.contains("bind")) result.bind = wire::string_field(json, "bind");
        const bool allow_remote = json.value("allow_remote", false);
        asio::error_code ec;
        const auto address = asio::ip::make_address(result.bind, ec);
        if (ec || (result.bind.find('\0') != result.bind.npos) ||
            (!address.is_loopback() && !allow_remote))
            throw StartupError("Invalid bind address or remote listening not enabled");

        result.port = static_cast<uint16_t>(number(json, "port", result.port, 0, 65535));
        result.max_connections = number(json, "max_connections", result.max_connections, 1, 128);
        result.timeout = std::chrono::milliseconds(number(json, "timeout_ms",
            static_cast<unsigned>(result.timeout.count()), 100, 120000));
        return result;
    }
    catch (const StartupError&) { throw; }
    catch (const std::exception&) { throw StartupError("Invalid configuration"); }
}

DataLock::DataLock(const Config& config)
{
    const auto path = config.data / ".sdp.lock";
    const auto file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw StartupError("Data directory lock unavailable");

    FILE_ATTRIBUTE_TAG_INFO info{};
    OVERLAPPED range{};
    if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &info, sizeof(info))
        || (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        || !LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &range))
    {
        CloseHandle(file);
        throw StartupError("Data directory already locked or inaccessible");
    }
    file_ = file;
}

DataLock::~DataLock()
{
    OVERLAPPED range{};
    UnlockFileEx(file_, 0, 1, 0, &range);
    CloseHandle(file_);
}

Admin load_admin(const Config& config)
{
    try
    {
        const auto json = wire::parse_payload(read_text(config.data / admin_file, 4096,
            "Administrator setup required. Use --bootstrap-admin."));
        Admin admin;
        admin.username = wire::string_field(json, "username");
        if (!username_valid(admin.username) || wire::string_field(json, "algorithm") != "pbkdf2-sha256"
            || json.at("iterations") != iterations)
            throw StartupError("Invalid administrator verifier");
        decode_hex(wire::string_field(json, "salt"), admin.salt);
        decode_hex(wire::string_field(json, "verifier"), admin.verifier);
        return admin;
    }
    catch (const StartupError&) { throw; }
    catch (const std::exception&) { throw StartupError("Invalid administrator verifier"); }
}

bool Admin::verify(std::string_view user, std::string_view password) const
{
    if (password.empty() || password.size() > max_password || password.find('\0') != password.npos) return false;
    std::array<unsigned char, 32> candidate{};
    derive(password, *this, candidate);
    const bool matches = CRYPTO_memcmp(candidate.data(), verifier.data(), verifier.size()) == 0;
    OPENSSL_cleanse(candidate.data(), candidate.size());
    return matches && user == username;
}

void Admin::set_password(std::string_view password)
{
    if (password.size() < 16 || password.size() > max_password || password.find('\0') != password.npos ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, password.data(), static_cast<int>(password.size()), nullptr, 0))
        throw StartupError("Password must be UTF-8 and between 16 and the configured byte limit");
    Admin candidate;
    if (RAND_bytes(candidate.salt.data(), static_cast<int>(candidate.salt.size())) != 1)
        throw StartupError("System random source failed");
    derive(password, candidate, candidate.verifier);
    salt = candidate.salt;
    verifier = candidate.verifier;
}

void bootstrap_admin(const Config& config, std::string_view username)
{
    if (!username_valid(username)) throw StartupError("Invalid administrator username");
    DataLock lock(config);
    if (std::filesystem::exists(config.data / admin_file)) throw StartupError("Administrator already configured");

    Secret password;
    Secret confirmation;
    ConsoleInput input;
    input.read(password, "Administrator password: ");
    input.read(confirmation, "Confirm administrator password: ");
    if (password.size < 16 || password.view().find('\0') != password.view().npos ||
        password.size != confirmation.size || CRYPTO_memcmp(password.bytes.data(), confirmation.bytes.data(), password.size))
        throw StartupError("Passwords must match and contain 16 to 1024 UTF-8 bytes");

    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, password.bytes.data(), static_cast<int>(password.size), nullptr, 0))
        throw StartupError("Password must be valid UTF-8");
    Admin admin;
    admin.username = username;
    admin.set_password(password.view());
    write_admin(config, admin);
    log_event(config, true, "administrator_created");
}

void log_event(const Config& config, bool auth, std::string_view event, std::string_view correlation) noexcept
{
    try
    {
        const auto file = config.logs / (auth ? "auth.log" : "operational.log");
        if (std::filesystem::exists(file) && std::filesystem::file_size(file) + 256 > log_limit)
        {
            if (!MoveFileExW(file.c_str(), (file.wstring() + L".1").c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw StartupError("Cannot rotate log");
        }
        const auto time = std::time(nullptr);
        std::tm utc{};
        gmtime_s(&utc, &time);
        char timestamp[32]{};
        std::strftime(timestamp, sizeof(timestamp), "%FT%TZ", &utc);
        std::ofstream output(file, std::ios::app | std::ios::binary);
        output << timestamp << ' ' << event;
        if (!correlation.empty()) output << ' ' << correlation;
        output << '\n';
        output.flush();
        if (!output) throw StartupError("Cannot write log");
    }
    catch (...)
    {
        static bool reported = false;
        if (!reported)
        {
            std::cerr << "Server log is unavailable\n";
            reported = true;
        }
    }
}
}
