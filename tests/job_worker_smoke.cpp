#include "worker.hpp"
#include "diagnostic.hpp"

#include <windows.h>

#include <fstream>
#include <iostream>

namespace
{
using namespace sdp;
using Status = jobs::WorkerResult::Status;

void require(bool passed, const char* message)
{
    if (!passed) throw std::runtime_error(message);
}

void dataset_calls(const std::filesystem::path& root, jobs::Budget budget)
{
    using wire::Json;
    constexpr auto source = "CREATE FUNCTION twice(p_count BIGINT) RETURN BIGINT AS BEGIN RETURN p_count * 2; END;";
    const auto number = [](std::string value) { return Json{{"t", "i64"}, {"v", std::move(value)}}; };
    const auto argument = [&](std::string value) { return Json{{"p_count", number(std::move(value))}}; };
    auto calls = Json::array({argument("21")});
    jobs::validate_dataset_routine(source);
    jobs::validate_dataset_calls(source, calls);
    Json request{{"kind", "dataset_plsql"}, {"source", source}, {"calls", calls}};
    bool complete{};
    auto result = jobs::run_worker(root, request, budget, [&](uint32_t pid, uint64_t done, uint64_t total) {
        require(pid && pid != GetCurrentProcessId() && total == 1, "Dataset routine did not use the worker process");
        complete = done == total;
        return true;
    });
    if (result.status != Status::succeeded) std::cerr << result.reason << '\n';
    require(result.status == Status::succeeded && complete && result.result["kind"] == "scalar" &&
        result.result["rows"] == Json::array({Json::array({number("42")})}), "Pure scalar job gave the wrong result");

    request["calls"] = Json::array({argument("21"), argument("-2"), argument("0")});
    result = jobs::run_worker(root, request, budget, {});
    require(result.status == Status::succeeded && result.result["kind"] == "result_set" &&
        result.result["rows"] == Json::array({Json::array({number("42")}), Json::array({number("-4")}),
            Json::array({number("0")})}), "Dataset call batch lost typed results or call order");

    request["source"] = "CREATE PROCEDURE summarize(seed IN BIGINT, doubled OUT BIGINT, positive OUT BOOLEAN) AS "
        "BEGIN doubled := seed * 2; positive := seed > 0; END;";
    request["calls"] = Json::array({Json{{"seed", number("21")}}});
    result = jobs::run_worker(root, request, budget, {});
    require(result.status == Status::succeeded && result.result["kind"] == "record" &&
        result.result["columns"] == Json::array({Json{{"name", "doubled"}, {"type", "BIGINT"}},
            Json{{"name", "positive"}, {"type", "BOOLEAN"}}}) &&
        result.result["rows"] == Json::array({Json::array({number("42"), Json{{"t", "bool"}, {"v", true}}})}),
        "Pure procedure OUT values lost their typed record schema");
    result = jobs::run_worker(root, request, {budget.timeout_ms, budget.memory_mib, 16}, {});
    require(result.status == Status::failed && result.reason == "output_limit" && result.result.is_null(),
        "Pure routine output quota published a partial record");

    request["source"] = "CREATE FUNCTION impure(seed BIGINT) RETURN BOOLEAN AS "
        "BEGIN RETURN EXISTS (SELECT 1 FROM other_data WHERE id=:seed); END;";
    bool rejected{};
    try { jobs::validate_dataset_routine(request["source"].get_ref<const std::string&>()); }
    catch (const sql::Error&) { rejected = true; }
    require(rejected, "SQL-hosted PL/SQL passed the worker's source validator");
    result = jobs::run_worker(root, request, budget, {});
    require(result.status == Status::failed && result.reason == "routine_failed" && result.result.is_null(),
        "Child did not independently reject SQL-hosted PL/SQL");

    request["source"] = source;
    request["calls"] = Json::array({Json{{"p_count", Json{{"t", "text"}, {"v", "21"}}}}});
    rejected = false;
    try { jobs::validate_dataset_calls(source, request["calls"]); }
    catch (const wire::Error&) { rejected = true; }
    require(rejected, "Worker accepted a wrongly typed routine input");
    require(std::filesystem::is_empty(root), "Dataset routine worker left source or result data behind");
}

void self_check(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root);
    const jobs::Budget budget{8000, 64, 1024};
    const wire::Json input{{"kind", "compute"}, {"count", 10'000}};
    bool complete{};
    auto result = jobs::run_worker(root, input, budget, [&](uint32_t pid, uint64_t done, uint64_t total) {
        require(pid && pid != GetCurrentProcessId() && total == 10'000 && done <= total, "Invalid child progress");
        complete = done == total;
        return true;
    });
    if (result.status != Status::succeeded) std::cerr << result.reason << '\n';
    require(result.status == Status::succeeded && complete && result.result == wire::Json{
        {"count", 10'000}, {"checksum", "15555538009459082212"}}, "Worker did not compute its reference result");
    require(std::filesystem::is_empty(root), "Success left transfer files");

    const wire::Json large{{"kind", "compute"}, {"count", 1'000'000'000}};
    result = jobs::run_worker(root, large, budget, [](uint32_t, uint64_t, uint64_t) { return false; });
    require(result.status == Status::cancelled && std::filesystem::is_empty(root), "Cancelled worker leaked scratch data");
    bool killed{};
    result = jobs::run_worker(root, large, budget, [&](uint32_t pid, uint64_t, uint64_t) {
        if (killed) return true;
        const auto handle = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        require(handle != nullptr, "Could not open isolated child to exercise process failure");
        killed = TerminateProcess(handle, 43) != FALSE;
        CloseHandle(handle);
        return true;
    });
    require(killed && result.status == Status::failed && result.reason == "worker_failed" &&
        std::filesystem::is_empty(root), "Early worker exit affected caller or leaked scratch data");

    result = jobs::run_worker(root, input, {8000, 64, 1}, {});
    require(result.status == Status::failed && result.reason == "output_limit" &&
        std::filesystem::is_empty(root), "Output quota failed to stop a result or clean up");
    result = jobs::run_worker(root, large, {100, 64, 1024}, {});
    require(result.status == Status::failed && result.reason == "timeout" &&
        std::filesystem::is_empty(root), "Time quota failed to stop a worker or clean up");
    require(!jobs::valid_worker_request({{"kind", "arbitrary"}, {"count", 1}}, budget),
        "Uncontrolled worker payload was accepted");

    const auto orphan = root / L"sdp-job-00000000000000000000000000000000";
    std::filesystem::create_directory(orphan);
    std::ofstream(orphan / L"owner.lock", std::ios::binary) << "SDP job workspace v1\n";
    std::ofstream(orphan / L"request.json", std::ios::binary) << "{}";
    const auto owner = CreateFileW((orphan / L"owner.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(owner != INVALID_HANDLE_VALUE, "Could not open owner marker");
    jobs::cleanup_worker_workspaces(root);
    const bool live_preserved = std::filesystem::is_directory(orphan);
    CloseHandle(owner);
    require(live_preserved, "Startup cleanup removed a live workspace");
    jobs::cleanup_worker_workspaces(root);
    require(std::filesystem::is_empty(root), "Startup cleanup left an orphan workspace");
    dataset_calls(root, budget);
    std::cout << "Worker compute, progress, crash, quotas, cleanup and typed PL/SQL dataset calls passed\n";
}
}

int wmain(int count, wchar_t** argv)
{
    if (count == 5 && std::wstring_view(argv[1]) == L"--job-worker")
        return jobs::worker_main(argv[2], argv[3], argv[4]);
    if (count != 2) return 2;
    try { self_check(argv[1]); }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
