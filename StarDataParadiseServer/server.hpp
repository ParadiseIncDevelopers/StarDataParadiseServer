#pragma once

#include <sdp/wire.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace sdp::server
{
inline constexpr auto product_version = "0.5.1";
struct Config
{
    std::filesystem::path source;
    std::filesystem::path data;
    std::filesystem::path logs;
    std::filesystem::path temp;
    std::filesystem::path backup;
    std::filesystem::path certificate;
    std::filesystem::path private_key;
    std::string instance_name = "default";
    std::string bind = "127.0.0.1";
    uint16_t port = 7443;
    unsigned max_connections = 16;
    bool routine_test_instance{};
    bool decision_enabled = true;
    bool ml_cpu_enabled = true;
    bool mlp_cpu_enabled = true;
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
int run(const Config& config, std::stop_token stop = {}, const std::function<void()>& ready = {});
uint16_t probe_update_listener(const Config& config);
}
