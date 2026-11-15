#include "worker.hpp"
#include "../ml/pl_dataset_job.hpp"
#include "../ml/training.hpp"
#include "../sql/diagnostic.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <openssl/rand.h>

namespace sdp::jobs
{
namespace
{
using Status = WorkerResult::Status;
using wire::Json;
using Path = std::filesystem::path;
using Clock = std::chrono::steady_clock;
constexpr uint64_t count_limit = 1'000'000'000;
constexpr DWORD poll_ms = 25;
constexpr DWORD cleanup_ms = 5000;
constexpr size_t request_limit = 1024;
constexpr size_t output_limit = 65'536;
constexpr size_t progress_limit = 100;
constexpr unsigned cpu_rate = 2'000; // 20% of the system, in hundredths of one percent.
constexpr int bad_transfer = 1, output_exceeded = 2, invalid_routine = 3;
constexpr std::wstring_view workspace_prefix = L"sdp-job-";
constexpr std::string_view owner_magic = "SDP job workspace v1\n";

class Handle
{
public:
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { if (*this) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};

bool plain_directory(const Path& path)
{
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool workspace_name(std::wstring_view name)
{
    return name.size() == workspace_prefix.size() + 32 && name.starts_with(workspace_prefix) &&
        name.find_first_not_of(L"0123456789abcdef", workspace_prefix.size()) == name.npos;
}

void remove_workspace(const Path& root) noexcept
{
    try
    {
        if (!plain_directory(root)) return;
        std::error_code ignored;
        for (const auto name : {L"request.json", L"result.json", L"progress.bin", L"owner.lock"})
            std::filesystem::remove(root / name, ignored);
        std::filesystem::remove(root, ignored);
    }
    catch (...) { }
}

class Workspace
{
public:
    explicit Workspace(const Path& temp)
    {
        if (!plain_directory(temp)) return;
        constexpr std::wstring_view hex = L"0123456789abcdef";
        for (unsigned retry = 0; retry < 4; ++retry)
        {
            std::array<unsigned char, 16> entropy{};
            if (RAND_bytes(entropy.data(), static_cast<int>(entropy.size())) != 1) return;
            std::wstring name(workspace_prefix);
            for (const auto byte : entropy)
            {
                name += hex[byte >> 4];
                name += hex[byte & 15];
            }
            auto directory = temp / name;
            std::error_code error;
            if (std::filesystem::create_directory(directory, error))
            {
                root = std::move(directory);
                marker_.emplace(CreateFileW((root / L"owner.lock").c_str(), GENERIC_WRITE, 0,
                    nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                DWORD written{};
                if (!*marker_ || !WriteFile(marker_->get(), owner_magic.data(),
                    static_cast<DWORD>(owner_magic.size()), &written, nullptr) || written != owner_magic.size())
                {
                    marker_.reset();
                    remove_workspace(root);
                    root.clear();
                }
                return;
            }
            if (error) return;
        }
    }

    ~Workspace()
    {
        if (root.empty()) return;
        marker_.reset();
        remove_workspace(root);
    }

    Path root;
private:
    std::optional<Handle> marker_;
};

bool plain_file(HANDLE file)
{
    BY_HANDLE_FILE_INFORMATION info{};
    return GetFileInformationByHandle(file, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

std::optional<std::string> read_bounded(const Path& path, size_t limit)
{
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    LARGE_INTEGER size{};
    if (!file || !plain_file(file.get()) || !GetFileSizeEx(file.get(), &size) || size.QuadPart < 1 ||
        static_cast<uint64_t>(size.QuadPart) > limit)
        return {};
    std::string contents(static_cast<size_t>(size.QuadPart), '\0');
    DWORD received{};
    if (!ReadFile(file.get(), contents.data(), static_cast<DWORD>(contents.size()), &received, nullptr) ||
        received != contents.size())
        return {};
    return contents;
}

bool write_new(const Path& path, std::string_view contents)
{
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    DWORD written{};
    return file && plain_file(file.get()) &&
        WriteFile(file.get(), contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) &&
        written == contents.size();
}

uint64_t count(const Json& input)
{
    if (!input.is_object() || input.size() != 2 || !input.contains("kind") || input["kind"] != "compute" ||
        !input.contains("count") || !input["count"].is_number_integer())
        return 0;
    const auto value = input["count"].get<uint64_t>();
    return value <= count_limit ? value : 0;
}

struct Work
{
    enum class Kind { compute, dataset, model };
    uint64_t total;
    Kind kind;
};

Work work_for(const Json& payload)
{
    if (const auto total = count(payload)) return {total, Work::Kind::compute};
    if (const auto total = ml::training_steps(payload)) return {total, Work::Kind::model};
    return {dataset_routine_calls(payload), Work::Kind::dataset};
}

bool valid_budget(Budget budget)
{
    return budget.timeout_ms >= 100 && budget.timeout_ms <= 60'000 && budget.memory_mib >= 16 &&
        budget.memory_mib <= 512 && budget.output_bytes >= 1 && budget.output_bytes <= output_limit;
}

WorkerResult failure(std::string reason, bool rate_limited = false)
{
    return {Status::failed, std::move(reason), {}, rate_limited};
}

Path executable()
{
    std::wstring path(32'768, L'\0');
    const auto written = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!written || written >= path.size()) return {};
    path.resize(written);
    return path;
}

std::wstring argument(const Path& path)
{
    // The fixed transfer filenames never end in a slash and Windows paths cannot contain a quote.
    return L"\"" + path.native() + L"\"";
}

void stop_worker(const Handle& job, const Handle& process)
{
    if (!TerminateJobObject(job.get(), 1)) TerminateProcess(process.get(), 1);
    WaitForSingleObject(process.get(), cleanup_ms);
}

bool valid_result(const Json& result, uint64_t expected)
{
    if (!result.is_object() || result.size() != 2 || !result.contains("count") ||
        !result["count"].is_number_unsigned() || result["count"] != expected ||
        !result.contains("checksum") || !result["checksum"].is_string())
        return false;
    const auto& text = result["checksum"].get_ref<const std::string&>();
    return !text.empty() && text.size() <= 20 && text.find_first_not_of("0123456789") == text.npos;
}

bool valid_dataset_result(const Json& result, uint64_t expected)
{
    if (!result.is_object() || result.size() != 3 || !result.contains("kind") ||
        !result.contains("columns") || !result["columns"].is_array() || result["columns"].empty() ||
        !result.contains("rows") || !result["rows"].is_array() || result["rows"].size() != expected)
        return false;
    const auto& kind = result["kind"];
    if (expected > 1 ? kind != "result_set" : kind != "scalar" && kind != "record") return false;
    const auto width = result["columns"].size();
    return std::ranges::all_of(result["rows"], [&](const Json& row) { return row.is_array() && row.size() == width; });
}

uint64_t progress_at(const Path& path, uint64_t done, uint64_t total)
{
    const auto data = read_bounded(path, sizeof(uint64_t) * progress_limit);
    if (!data || data->size() % sizeof(uint64_t)) return done;
    uint64_t value{};
    const auto offset = data->size() - sizeof(uint64_t);
    for (size_t i = 0; i < sizeof(uint64_t); ++i)
        value |= static_cast<uint64_t>(static_cast<unsigned char>((*data)[offset + i])) << (8 * i);
    return value > done && value <= total ? value : done;
}

bool write_progress(HANDLE file, uint64_t done)
{
    std::array<char, sizeof(uint64_t)> encoded{};
    for (size_t i = 0; i < encoded.size(); ++i) encoded[i] = static_cast<char>(done >> (8 * i));
    DWORD written{};
    return WriteFile(file, encoded.data(), static_cast<DWORD>(encoded.size()), &written, nullptr) &&
        written == encoded.size();
}

WorkerResult process_worker(const Path& workspace, const Json& payload, Budget budget,
    const Progress& progress, Work work)
{
    const auto request = workspace / L"request.json";
    const auto result = workspace / L"result.json";
    const auto status = workspace / L"progress.bin";
    const auto body = Json{{"payload", payload}, {"output_bytes", budget.output_bytes}}.dump();
    const auto transfer_limit = work.kind == Work::Kind::compute ? request_limit : dataset_worker_request_limit;
    if (body.size() > transfer_limit || !write_new(request, body)) return failure("transfer_failed");

    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) return failure("resource_unavailable");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    auto& basic = limits.BasicLimitInformation;
    basic.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY |
        JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_PROCESS_TIME | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    basic.ActiveProcessLimit = 1;
    basic.PerProcessUserTimeLimit.QuadPart = static_cast<int64_t>(budget.timeout_ms) * 10'000;
    limits.ProcessMemoryLimit = static_cast<SIZE_T>(budget.memory_mib) * 1024 * 1024;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return failure("resource_unavailable");
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
    cpu.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
    cpu.CpuRate = cpu_rate;
    const bool rate = SetInformationJobObject(job.get(), JobObjectCpuRateControlInformation, &cpu, sizeof(cpu)) != FALSE;

    const auto image = executable();
    if (image.empty()) return failure("resource_unavailable", rate);
    auto command = argument(image) + L" --job-worker " + argument(request) + L" " +
        argument(result) + L" " + argument(status);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    std::array<wchar_t, 2> environment{};
    if (!CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        environment.data(), workspace.c_str(), &startup, &info))
        return failure("resource_unavailable", rate);
    Handle process(info.hProcess), thread(info.hThread);
    if (!AssignProcessToJobObject(job.get(), process.get()))
    {
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), cleanup_ms);
        return failure("resource_unavailable", rate);
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(budget.timeout_ms);
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1))
    {
        stop_worker(job, process);
        return failure("worker_failed", rate);
    }
    uint64_t done{};
    for (;;)
    {
        const auto wait = WaitForSingleObject(process.get(), poll_ms);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_TIMEOUT)
        {
            stop_worker(job, process);
            return failure("worker_failed", rate);
        }
        done = progress_at(status, done, work.total);
        if (progress && !progress(info.dwProcessId, done, work.total))
        {
            stop_worker(job, process);
            return {Status::cancelled, "cancelled", {}, rate};
        }
        if (Clock::now() >= deadline)
        {
            stop_worker(job, process);
            return failure("timeout", rate);
        }
    }

    DWORD code{};
    if (!GetExitCodeProcess(process.get(), &code)) return failure("worker_failed", rate);
    if (code == output_exceeded) return failure("output_limit", rate);
    if (code == invalid_routine) return failure(work.kind == Work::Kind::model ? "training_failed" : "routine_failed", rate);
    if (code != 0) return failure("worker_failed", rate);
    const auto bytes = read_bounded(result, budget.output_bytes);
    if (!bytes) return failure("transfer_failed", rate);
    auto output = Json::parse(*bytes, nullptr, false);
    const bool valid = work.kind == Work::Kind::model ?
        ml::valid_training_transfer(output, payload) :
        work.kind == Work::Kind::dataset ? valid_dataset_result(output, work.total) : valid_result(output, work.total);
    if (!valid) return failure("transfer_failed", rate);
    if (progress) (void)progress(info.dwProcessId, work.total, work.total);
    return {Status::succeeded, {}, std::move(output), rate};
}

bool valid_paths(const Path& request, const Path& result, const Path& progress)
{
    const auto parent = request.parent_path();
    return request.is_absolute() && result.parent_path() == parent && progress.parent_path() == parent &&
        request.filename() == L"request.json" && result.filename() == L"result.json" && progress.filename() == L"progress.bin" &&
        workspace_name(parent.filename().native()) && plain_directory(parent);
}

Json run_compute(uint64_t total, const std::function<void(uint64_t)>& progress)
{
    const auto step = (total + progress_limit - 1) / progress_limit;
    auto next = step;
    uint64_t checksum = 0x9e3779b97f4a7c15ULL;
    for (uint64_t iteration = 1; iteration <= total; ++iteration)
    {
        checksum ^= iteration + 0x9e3779b97f4a7c15ULL;
        checksum = (checksum ^ (checksum >> 30)) * 0xbf58476d1ce4e5b9ULL;
        checksum = (checksum ^ (checksum >> 27)) * 0x94d049bb133111ebULL;
        checksum ^= checksum >> 31;
        if (iteration == next)
        {
            progress(iteration);
            next += step;
        }
    }
    return {{"count", total}, {"checksum", std::to_string(checksum)}};
}
}

bool valid_worker_request(const wire::Json& payload, Budget budget)
{
    const auto work = work_for(payload);
    return work.total && valid_budget(budget) &&
        (work.kind == Work::Kind::compute || payload.dump().size() <= dataset_worker_request_limit - 128);
}

void cleanup_worker_workspaces(const std::filesystem::path& temp) noexcept
{
    try
    {
        if (!plain_directory(temp)) return;
        std::error_code error;
        auto item = std::filesystem::directory_iterator(temp, error);
        const auto end = std::filesystem::directory_iterator{};
        for (; !error && item != end; item.increment(error))
        {
            const auto& path = item->path();
            if (!workspace_name(path.filename().native()) || !plain_directory(path)) continue;
            bool stale{};
            {
                Handle owner(CreateFileW((path / L"owner.lock").c_str(), GENERIC_READ, 0,
                    nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                std::array<char, owner_magic.size() + 1> magic{};
                DWORD received{};
                stale = owner && plain_file(owner.get()) &&
                    ReadFile(owner.get(), magic.data(), static_cast<DWORD>(magic.size()), &received, nullptr) &&
                    std::string_view(magic.data(), received) == owner_magic;
            }
            if (stale) remove_workspace(path);
        }
    }
    catch (...) { }
}

WorkerResult run_worker(const std::filesystem::path& temp, const wire::Json& payload, Budget budget, const Progress& progress)
{
    const auto work = work_for(payload);
    if (!work.total || !valid_budget(budget)) return failure("invalid_payload");
    try
    {
        Workspace workspace(std::filesystem::absolute(temp).lexically_normal());
        if (workspace.root.empty()) return failure("transfer_failed");
        return process_worker(workspace.root, payload, budget, progress, work);
    }
    catch (const std::filesystem::filesystem_error&) { return failure("transfer_failed"); }
    catch (const Json::exception&) { return failure("transfer_failed"); }
}

int worker_main(const std::filesystem::path& request, const std::filesystem::path& result,
    const std::filesystem::path& progress)
{
    try
    {
        if (!valid_paths(request, result, progress) || GetFileAttributesW(result.c_str()) != INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesW(progress.c_str()) != INVALID_FILE_ATTRIBUTES)
            return bad_transfer;
        const auto bytes = read_bounded(request, dataset_worker_request_limit);
        if (!bytes) return bad_transfer;
        const auto body = Json::parse(*bytes, nullptr, false);
        if (!body.is_object() || body.size() != 2 || !body.contains("payload") ||
            !body.contains("output_bytes") || !body["output_bytes"].is_number_unsigned())
            return bad_transfer;
        const auto work = work_for(body["payload"]);
        const auto budget = body["output_bytes"].get<uint64_t>();
        if (!work.total || !budget || budget > output_limit ||
            (work.kind == Work::Kind::compute && bytes->size() > request_limit))
            return bad_transfer;
        Handle journal(CreateFileW(progress.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!journal || !plain_file(journal.get())) return bad_transfer;
        const auto report = [&](uint64_t done) {
            if (!write_progress(journal.get(), done)) throw std::runtime_error("Worker progress transfer failed");
        };
        std::optional<Json> output;
        switch (work.kind)
        {
        case Work::Kind::compute: output = run_compute(work.total, report); break;
        case Work::Kind::dataset: output = run_dataset_routine(body["payload"], static_cast<size_t>(budget), report); break;
        case Work::Kind::model: output = ml::train_model(body["payload"], report); break;
        }
        if (!output) return output_exceeded;
        const auto answer = output->dump();
        if (answer.size() > budget) return output_exceeded;
        return write_new(result, answer) ? 0 : bad_transfer;
    }
    catch (const sql::Error&) { return invalid_routine; }
    catch (const wire::Error&) { return invalid_routine; }
    catch (const std::exception&) { return bad_transfer; }
}
}
