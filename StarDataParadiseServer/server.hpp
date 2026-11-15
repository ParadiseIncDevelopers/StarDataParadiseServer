#pragma once

#include <sdp/wire.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sdp::server
{
inline constexpr auto product_version = "0.2.0";
struct Config
{
    std::filesystem::path data;
    std::filesystem::path logs;
    std::filesystem::path temp;
    std::filesystem::path certificate;
    std::filesystem::path private_key;
    std::string bind = "127.0.0.1";
    uint16_t port = 7443;
    unsigned max_connections = 16;
    bool routine_test_instance{};
    std::chrono::milliseconds timeout{10000};
};

class StartupError : public std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct Admin
{
    std::string username;
    std::array<unsigned char, 16> salt{};
    std::array<unsigned char, 32> verifier{};

    bool verify(std::string_view user, std::string_view password) const;
    void set_password(std::string_view password);
};

class DataLock
{
public:
    explicit DataLock(const Config& config);
    ~DataLock();
    DataLock(const DataLock&) = delete;
    DataLock& operator=(const DataLock&) = delete;

private:
    void* file_;
};

Config read_config(const std::filesystem::path& path);
Admin load_admin(const Config& config);
void bootstrap_admin(const Config& config, std::string_view username);
void log_event(const Config& config, bool auth, std::string_view event, std::string_view correlation = {}) noexcept;
int run(const Config& config);
}
