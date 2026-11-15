#include "windows_service.hpp"

#include <windows.h>
#include <winsvc.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <iostream>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

namespace sdp::server
{
namespace
{
using namespace std::chrono_literals;
constexpr auto accepted_controls = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
constexpr auto service_timeout = 45s;
constexpr auto wait_hint = 30'000u;

struct CloseService
{
    void operator()(SC_HANDLE handle) const noexcept { CloseServiceHandle(handle); }
};
using Handle = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, CloseService>;

[[noreturn]] void service_error(std::string_view operation, DWORD error = GetLastError())
{
    throw StartupError(std::string(operation) + " (Windows error " + std::to_string(error) + ")");
}

std::wstring service_name(std::wstring_view instance)
{
    if (instance.empty() || instance.size() > 48 ||
        instance.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != instance.npos)
        throw StartupError("Invalid Windows service instance name");
    return L"StarDataParadise-" + std::wstring(instance);
}

std::filesystem::path canonical_file(const std::filesystem::path& path)
{
    std::error_code error;
    auto result = std::filesystem::canonical(path, error);
    if (error || !result.is_absolute() || !std::filesystem::is_regular_file(result, error) || error)
        throw StartupError("Windows service executable or configuration is unavailable");
    return result;
}

std::filesystem::path executable_file()
{
    std::wstring raw(32'768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, raw.data(), static_cast<DWORD>(raw.size()));
    if (!count || count >= raw.size()) service_error("Cannot locate the running server executable");
    raw.resize(count);
    return canonical_file(raw);
}

std::wstring quote_path(const std::filesystem::path& path)
{
    const auto& text = path.native();
    if (text.empty() || text.find_first_of(L"\"\r\n") != text.npos || text.back() == L'\\')
        throw StartupError("Invalid Windows service command path");
    return L"\"" + text + L"\"";
}

bool equal_name(const wchar_t* left, const wchar_t* right)
{
    return CompareStringOrdinal(left, -1, right, -1, TRUE) == CSTR_EQUAL;
}

SERVICE_STATUS_PROCESS status_of(SC_HANDLE service)
{
    SERVICE_STATUS_PROCESS result{};
    DWORD used{};
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&result), sizeof(result), &used))
        service_error("Cannot query Windows service status");
    return result;
}

void require_registration(SC_HANDLE service, const std::wstring& command, const std::wstring& account)
{
    alignas(QUERY_SERVICE_CONFIGW) std::array<std::byte, 8192> data{};
    auto* configuration = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(data.data());
    DWORD used{};
    if (!QueryServiceConfigW(service, configuration, static_cast<DWORD>(data.size()), &used))
        service_error("Cannot verify Windows service registration");
    if (configuration->dwServiceType != SERVICE_WIN32_OWN_PROCESS ||
        !configuration->lpBinaryPathName || command != configuration->lpBinaryPathName ||
        !configuration->lpServiceStartName || !equal_name(account.c_str(), configuration->lpServiceStartName))
        throw StartupError("Windows service registration does not match this executable, configuration and identity");
}

const char* state_name(DWORD state)
{
    switch (state)
    {
    case SERVICE_STOPPED: return "stopped";
    case SERVICE_START_PENDING: return "start_pending";
    case SERVICE_STOP_PENDING: return "stop_pending";
    case SERVICE_RUNNING: return "running";
    case SERVICE_CONTINUE_PENDING: return "continue_pending";
    case SERVICE_PAUSE_PENDING: return "pause_pending";
    case SERVICE_PAUSED: return "paused";
    default: return "unknown";
    }
}

void print_status(std::string_view instance, std::string_view state, DWORD process = 0, DWORD error = 0)
{
    std::cout << wire::Json{{"instance", instance}, {"state", state}, {"process_id", process},
        {"windows_exit_code", error}}.dump() << '\n';
}

SERVICE_STATUS_PROCESS await_state(SC_HANDLE service, DWORD target)
{
    const auto until = std::chrono::steady_clock::now() + service_timeout;
    for (;;)
    {
        const auto status = status_of(service);
        if (status.dwCurrentState == target) return status;
        if (target == SERVICE_RUNNING && status.dwCurrentState == SERVICE_STOPPED)
            service_error("Windows service stopped during startup", status.dwWin32ExitCode ?
                status.dwWin32ExitCode : ERROR_SERVICE_NOT_ACTIVE);
        if (std::chrono::steady_clock::now() >= until)
            service_error("Timed out waiting for the Windows service", ERROR_TIMEOUT);
        std::this_thread::sleep_for(100ms);
    }
}

class Runtime;
std::atomic<Runtime*> active_runtime{};

class Runtime
{
public:
    Runtime(std::filesystem::path config, std::wstring name, std::wstring_view instance)
        : config_(std::move(config)), name_(std::move(name)), instance_(instance) {}

    int dispatch()
    {
        Runtime* empty = nullptr;
        if (!active_runtime.compare_exchange_strong(empty, this))
            throw StartupError("Windows service dispatcher already started");
        SERVICE_TABLE_ENTRYW table[]{{name_.data(), &entry}, {nullptr, nullptr}};
        const auto dispatched = StartServiceCtrlDispatcherW(table);
        const auto error = GetLastError();
        active_runtime.store(nullptr);
        if (!dispatched) service_error("Cannot connect to the Windows Service Control Manager", error);
        return result_;
    }

private:
    static void WINAPI entry(DWORD argc, wchar_t** argv) noexcept
    {
        if (auto* runtime = active_runtime.load()) runtime->serve(argc, argv);
    }

    static DWORD WINAPI control(DWORD event, DWORD, void*, void* context) noexcept
    {
        auto& service = *static_cast<Runtime*>(context);
        if (event == SERVICE_CONTROL_INTERROGATE) return NO_ERROR;
        if (event != SERVICE_CONTROL_STOP && event != SERVICE_CONTROL_SHUTDOWN) return ERROR_CALL_NOT_IMPLEMENTED;
        const std::lock_guard guard(service.mutex_);
        if (service.state_ == SERVICE_RUNNING)
        {
            service.report(SERVICE_STOP_PENDING);
            service.stop_.request_stop();
        }
        return NO_ERROR;
    }

    bool report(DWORD next, DWORD error = NO_ERROR) noexcept
    {
        SERVICE_STATUS status{};
        status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        status.dwCurrentState = next;
        status.dwControlsAccepted = next == SERVICE_RUNNING ? accepted_controls : 0;
        status.dwWin32ExitCode = error ? static_cast<DWORD>(ERROR_SERVICE_SPECIFIC_ERROR) : NO_ERROR;
        status.dwServiceSpecificExitCode = error;
        if (next == SERVICE_START_PENDING || next == SERVICE_STOP_PENDING)
        {
            status.dwCheckPoint = 1;
            status.dwWaitHint = wait_hint;
        }
        state_ = next;
        return SetServiceStatus(handle_, &status) != 0;
    }

    void serve(DWORD argc, wchar_t** argv) noexcept
    {
        result_ = 1;
        if (argc != 1 || !argv || !argv[0] || !equal_name(name_.c_str(), argv[0])) return;
        handle_ = RegisterServiceCtrlHandlerExW(name_.c_str(), &control, this);
        if (!handle_) return;
        try
        {
            {
                const std::lock_guard guard(mutex_);
                if (!report(SERVICE_START_PENDING)) service_error("Cannot report start status to Windows");
            }
            const auto config = read_config(config_);
            if (std::wstring(config.instance_name.begin(), config.instance_name.end()) != instance_)
                throw StartupError("Windows service configuration instance changed");
            try
            {
                result_ = run(config, stop_.get_token(), [this] {
                    const std::lock_guard guard(mutex_);
                    if (!report(SERVICE_RUNNING)) service_error("Cannot report running status to Windows");
                });
            }
            catch (...) { log_event(config, false, "windows_service_runtime_failed"); throw; }
        }
        catch (...) { result_ = 1; }
        const std::lock_guard guard(mutex_);
        report(SERVICE_STOPPED, static_cast<DWORD>(result_));
    }

    std::filesystem::path config_;
    std::wstring name_;
    std::wstring instance_;
    std::mutex mutex_;
    std::stop_source stop_;
    SERVICE_STATUS_HANDLE handle_{};
    DWORD state_ = SERVICE_START_PENDING;
    std::atomic_int result_{1};
};
}

int windows_service(std::wstring_view operation, const std::filesystem::path& config_path, const Config& config)
{
    const auto install = operation == L"install";
    const auto query = operation == L"status";
    const auto start = operation == L"start";
    const auto stop = operation == L"stop";
    const auto remove = operation == L"remove";
    if (!install && !query && !start && !stop && !remove) throw StartupError("Invalid Windows service operation");

    const std::wstring instance(config.instance_name.begin(), config.instance_name.end());
    const auto name = service_name(instance);
    const auto account = L"NT SERVICE\\" + name;
    const auto command = quote_path(executable_file()) + L" --service-run " +
        quote_path(canonical_file(config_path)) + L" " + instance;
    if (command.size() > 3'500) throw StartupError("Windows service command exceeds the supported length");

    Handle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | (install ? SC_MANAGER_CREATE_SERVICE : 0)));
    if (!manager) service_error("Cannot open the local Windows Service Control Manager");
    if (install)
    {
        Handle service(CreateServiceW(manager.get(), name.c_str(), name.c_str(), SERVICE_QUERY_STATUS,
            SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, command.c_str(),
            nullptr, nullptr, nullptr, account.c_str(), nullptr));
        if (!service) service_error("Cannot install Windows service");
        print_status(config.instance_name, "installed");
        return 0;
    }

    const DWORD action_access = start ? SERVICE_START : stop ? SERVICE_STOP : remove ? DELETE : 0;
    Handle service(OpenServiceW(manager.get(), name.c_str(), SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG | action_access));
    if (!service)
    {
        const auto error = GetLastError();
        if (query && error == ERROR_SERVICE_DOES_NOT_EXIST)
        {
            print_status(config.instance_name, "not_installed");
            return 0;
        }
        service_error("Cannot open the mapped Windows service", error);
    }
    require_registration(service.get(), command, account);
    auto status = status_of(service.get());
    if (start)
    {
        if (status.dwCurrentState == SERVICE_STOPPED && !StartServiceW(service.get(), 0, nullptr) &&
            GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
            service_error("Cannot start Windows service");
        if (status.dwCurrentState != SERVICE_RUNNING)
            status = await_state(service.get(), SERVICE_RUNNING);
    }
    if (stop && status.dwCurrentState != SERVICE_STOPPED)
    {
        SERVICE_STATUS stopping{};
        if (status.dwCurrentState != SERVICE_STOP_PENDING &&
            !ControlService(service.get(), SERVICE_CONTROL_STOP, &stopping) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE)
            service_error("Cannot stop Windows service");
        status = await_state(service.get(), SERVICE_STOPPED);
    }
    if (remove)
    {
        if (status.dwCurrentState != SERVICE_STOPPED) throw StartupError("Stop the Windows service before removing it");
        if (!DeleteService(service.get())) service_error("Cannot remove Windows service");
        print_status(config.instance_name, "marked_for_delete");
        return 0;
    }
    print_status(config.instance_name, state_name(status.dwCurrentState), status.dwProcessId, status.dwWin32ExitCode);
    return 0;
}

int run_windows_service(const std::filesystem::path& config_path, std::wstring_view instance)
{
    if (!config_path.is_absolute()) throw StartupError("Windows service configuration path must be absolute");
    Runtime service(config_path, service_name(instance), instance);
    return service.dispatch();
}
}

