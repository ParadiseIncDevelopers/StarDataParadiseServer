#include "devices.hpp"

#include "../sql/wire_values.hpp"

#include <Windows.h>
#include <array>

namespace sdp::ml
{
namespace
{
using wire::Json;
using W = wire::Code;

struct Driver
{
    HMODULE module = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    ~Driver() { if (module) FreeLibrary(module); }
    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;
    Driver() = default;
};

struct Probe
{
    Json devices = Json::array();
    std::string_view reason = "cuda_driver_unavailable";
    int driver_version{};
};

Probe discover()
{
    Driver driver;
    Probe result;
    if (!driver.module) return result;
    using Init = int (WINAPI*)(unsigned);
    using GetInt = int (WINAPI*)(int*);
    using GetDevice = int (WINAPI*)(int*, int);
    using GetMemory = int (WINAPI*)(size_t*, int);
#pragma warning(push)
#pragma warning(disable:4191)
    const auto init = reinterpret_cast<Init>(GetProcAddress(driver.module, "cuInit"));
    const auto version = reinterpret_cast<GetInt>(GetProcAddress(driver.module, "cuDriverGetVersion"));
    const auto count = reinterpret_cast<GetInt>(GetProcAddress(driver.module, "cuDeviceGetCount"));
    const auto device = reinterpret_cast<GetDevice>(GetProcAddress(driver.module, "cuDeviceGet"));
    const auto memory = reinterpret_cast<GetMemory>(GetProcAddress(driver.module, "cuDeviceTotalMem_v2"));
#pragma warning(pop)
    result.reason = "cuda_driver_incompatible";
    if (!init || !version || !count || !device || !memory ||
        init(0) != 0 || version(&result.driver_version) != 0 || result.driver_version < 12000) return result;
    int available{};
    if (count(&available) != 0) return result;
    result.reason = available > 0 ? "gpu_backend_unavailable" : "cuda_device_unavailable";
    for (int index = 0; index < available && index < 32; ++index)
    {
        int handle{};
        size_t bytes{};
        if (device(&handle, index) == 0 && memory(&bytes, handle) == 0)
            result.devices.push_back({{"index", index}, {"memory_mib", bytes / (1024 * 1024)}});
    }
    return result;
}

const Probe& probe()
{
    static const auto result = discover();
    return result;
}

std::string_view device_reason(uint32_t index, uint32_t memory)
{
    const auto& report = probe();
    if (report.reason != "gpu_backend_unavailable") return report.reason;
    for (const auto& device : report.devices)
        if (device.at("index") == index)
            return device.at("memory_mib").get<uint64_t>() < memory ? "gpu_memory_budget_unavailable" : report.reason;
    return "selected_gpu_unavailable";
}
}

Json model_devices(const Json& body, bool cpu_available)
{
    for (const auto& field : body.items())
        if (field.key() != "device_index" && field.key() != "memory_mib")
            throw wire::Error(W::invalid_argument);
    const auto index = sql::unsigned_field(body, "device_index", 31, 0);
    const auto memory = sql::unsigned_field(body, "memory_mib", 131072, 128);
    if (!memory) throw wire::Error(W::invalid_argument);
    const auto& report = probe();
    return {{"cpu_available", cpu_available}, {"gpu_available", false}, {"gpu_verified", false},
        {"gpu_reason", device_reason(index, memory)}, {"driver_api", "cuda"},
        {"driver_version", report.driver_version ? Json(report.driver_version) : Json(nullptr)},
        {"device_index", index}, {"memory_mib", memory}, {"devices", report.devices}};
}

Json choose_device(const Json& body)
{
    const auto device = body.contains("device") ? std::string_view(wire::string_field(body, "device")) : "cpu";
    if (device == "cpu")
    {
        if (body.contains("device_index") || body.contains("memory_mib") || body.contains("allow_cpu_fallback"))
            throw wire::Error(W::invalid_argument);
        return nullptr;
    }
    if (device != "gpu" || (body.contains("allow_cpu_fallback") && !body.at("allow_cpu_fallback").is_boolean()))
        throw wire::Error(W::invalid_argument);
    const auto index = sql::unsigned_field(body, "device_index", 31, 0);
    const auto memory = sql::unsigned_field(body, "memory_mib", 131072, 128);
    if (!memory) throw wire::Error(W::invalid_argument);
    if (!body.value("allow_cpu_fallback", false)) throw wire::Error(W::unsupported);
    return {{"requested_device", "gpu"}, {"requested_device_index", index}, {"requested_memory_mib", memory},
        {"fallback_reason", device_reason(index, memory)}};
}
}
