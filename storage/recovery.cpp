#include "recovery.hpp"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <utility>

namespace sdp::disk
{
namespace
{
constexpr std::array<Byte, 8> checkpoint_magic{'S','D','P','C','K','P','1',0};

bool present(const std::filesystem::path& path)
{
    return std::filesystem::symlink_status(path).type() != std::filesystem::file_type::not_found;
}

std::filesystem::path checkpoint_path(std::filesystem::path data)
{
    data += ".checkpoint";
    return data;
}

void verify_checkpoint(const std::filesystem::path& path, uint64_t last)
{
    if (!present(path)) return;
    File file(path, File::Mode::read_only);
    Page record{};
    if (file.size() != page_size) throw Error(Fault::corrupt, "Invalid checkpoint length");
    file.read(0, record);
    const auto checkpoint = load_le<uint64_t>(record, 16);
    const auto floor = load_le<uint64_t>(record, 24);
    if (!sealed(record) || !std::ranges::equal(checkpoint_magic, std::span(record).first(checkpoint_magic.size())) ||
        load_le<uint32_t>(record, 8) != 1 || load_le<uint32_t>(record, 12) ||
        checkpoint > last || !floor || floor > checkpoint + 1 ||
        !std::ranges::all_of(std::span(record).subspan(32, page_end - 32), [](Byte byte) { return byte == 0; }))
        throw Error(Fault::corrupt, "Checkpoint metadata is incompatible with the retained WAL");
}

struct Redo
{
    Header target;
    PageMap images;
    std::map<RowId, Bytes> ledger;
    std::map<std::pair<PageId, uint64_t>, uint32_t> image_checksums;
    std::map<uint64_t, PageId> published{{0,1}};

    void accept(Frame frame)
    {
        const auto next = frame.batch.header;
        if (next.lsn != target.lsn + 1 || next.pages < target.pages)
            throw Error(Fault::corrupt, "WAL publication sequence is invalid");
        for (auto id = target.pages; id < next.pages; ++id)
            if (!frame.batch.pages.contains(id)) throw Error(Fault::corrupt, "WAL allocation is missing a page image");
        for (auto& [id, image] : frame.batch.pages)
        {
            image_checksums.emplace(std::pair{id, next.lsn}, load_le<uint32_t>(image, page_end));
            images.insert_or_assign(id, std::move(image));
        }
        for (auto& change : frame.changes)
        {
            const auto found = ledger.find(change.id);
            // An insert opens a new identity; a restore can open a previously verified tombstone.
            if (change.before.empty() ? found != ledger.end() && !found->second.empty() :
                found == ledger.end() || found->second != change.before)
                throw Error(Fault::corrupt, "Transaction journal chain is inconsistent");
            ledger.insert_or_assign(change.id, std::move(change.after));
        }
        published.emplace(next.lsn, next.pages);
        target = next;
    }

    Integrity inspect(std::span<const RowId> index_candidates) const
    {
        size_t live{};
        for (const auto& [id, row] : ledger)
            if (!row.empty()) ++live;
        const auto summary = inspect_pages(target, [this](PageId id) {
            const auto match = images.find(id);
            if (match == images.end()) throw Error(Fault::corrupt, "No committed image for a storage page");
            return match->second;
        }, [this](RowId id, const Row& row) {
            const auto found = ledger.find(id);
            if (found == ledger.end() || found->second != encode(row))
                throw Error(Fault::corrupt, "Published row and transaction journal differ");
        }, index_candidates);
        if (summary.rows != live) throw Error(Fault::corrupt, "Transaction journal has a missing row");
        return summary;
    }
};

Scan read_all(const std::vector<Segment>& files, Redo& redo)
{
    Scan latest{};
    uint64_t total{};
    for (size_t index = 0; index < files.size(); ++index)
    {
        const auto& segment = files[index];
        if (segment.first != redo.target.lsn + 1) throw Error(Fault::corrupt, "A required WAL segment is missing");
        File file(segment.path, File::Mode::read_only);
        const auto bytes = file.size();
        if (bytes > wal_quota - total) throw Error(Fault::corrupt, "Retained WAL exceeds its quota");
        total += bytes;
        latest = scan(file, [&](Frame&& frame) { redo.accept(std::move(frame)); });
        if (latest.first != segment.first || (index + 1 < files.size() && (latest.incomplete_tail || !latest.commits)))
            throw Error(Fault::corrupt, "An inactive WAL segment is invalid");
    }
    return latest;
}

std::vector<PageId> differences(File& data, const Redo& redo)
{
    std::vector<PageId> dirty;
    const auto size = data.size();
    if (size > uint64_t{redo.target.pages} * page_size)
        throw Error(Fault::corrupt, "Storage extends beyond committed WAL");

    Page image{};
    if (size >= page_size)
    {
        data.read(0, image);
        if (sealed(image))
        {
            const auto stored = read_header(image);
            const auto revision = redo.published.find(stored.lsn);
            if (revision == redo.published.end() || stored.pages != revision->second)
                throw Error(Fault::corrupt, "Storage header has no committed WAL revision");
        }
    }
    if (image != make_header(redo.target)) dirty.push_back(0);

    for (const auto& [id, expected] : redo.images)
    {
        const auto start = uint64_t{id} * page_size;
        if (size >= start + page_size)
        {
            data.read(start, image);
            if (image == expected) continue;
            if (sealed(image))
            {
                validate_page(image, id, redo.target.lsn);
                const auto old = load_le<uint64_t>(image, 12);
                const auto lsn = load_le<uint64_t>(expected, 12);
                const auto known = redo.image_checksums.find({id, old});
                if (old >= lsn || known == redo.image_checksums.end() || known->second != load_le<uint32_t>(image, page_end))
                    throw Error(Fault::corrupt, "A valid page conflicts with the committed WAL image");
            }
        }
        dirty.push_back(id);
    }
    return dirty;
}
}

std::vector<Segment> segments(const std::filesystem::path& data)
{
    const auto folder = data.parent_path().empty() ? std::filesystem::path(".") : data.parent_path();
    const auto prefix = data.filename().wstring() + L".wal.";
    std::vector<Segment> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder))
    {
        const auto filename = entry.path().filename().wstring();
        if (!filename.starts_with(prefix)) continue;
        const std::wstring_view suffix(filename.data() + prefix.size(), filename.size() - prefix.size());
        if (suffix.size() != 20 || suffix.find_first_not_of(L"0123456789") != suffix.npos)
            throw Error(Fault::corrupt, "Invalid WAL segment file name");
        std::string digits;
        for (const auto digit : suffix) digits.push_back(static_cast<char>(digit));
        uint64_t start{};
        const auto parse = std::from_chars(digits.data(), digits.data() + digits.size(), start);
        if (parse.ec != std::errc{} || !start || start == UINT64_MAX)
            throw Error(Fault::corrupt, "Invalid WAL segment LSN");
        files.push_back({entry.path(), start});
    }
    std::ranges::sort(files, {}, &Segment::first);
    return files;
}

Recovery verify_database(const std::filesystem::path& data, bool repair, std::span<const RowId> index_candidates)
{
    const auto files = segments(data);
    const auto checkpoint = checkpoint_path(data);
    if (files.empty())
    {
        if (present(checkpoint)) throw Error(Fault::corrupt, "Checkpoint exists without retained WAL");
        Store legacy(data, true);
        return {legacy.header(), legacy.inspect(index_candidates)};
    }

    File storage(data, repair ? File::Mode::read_write : File::Mode::read_only);
    Redo redo;
    const auto latest = read_all(files, redo);
    verify_checkpoint(checkpoint, redo.target.lsn);
    const auto summary = redo.inspect(index_candidates);
    const auto dirty = differences(storage, redo);
    if ((!dirty.empty() || latest.incomplete_tail) && !repair)
        throw Error(Fault::uncertain, "Committed WAL requires recovery before inspection");
    if (!dirty.empty() && !redo.target.lsn)
        throw Error(Fault::corrupt, "No committed WAL can repair the storage header");

    if (repair && (!dirty.empty() || latest.incomplete_tail))
    {
        for (size_t index = 0; index < files.size(); ++index)
        {
            File wal(files[index].path, File::Mode::read_write);
            if (index + 1 == files.size() && latest.incomplete_tail) wal.resize(latest.valid_end);
            wal.flush();
        }
        for (const auto id : dirty)
            if (id != 0) storage.write(uint64_t{id} * page_size, redo.images.at(id));
        if (!dirty.empty())
        {
            storage.write(0, make_header(redo.target));
            storage.flush();
        }
    }
    return {redo.target, summary, redo.published.size() - 1, dirty.size(), latest.incomplete_tail};
}

Checkpoint retention(uint64_t published, uint64_t history, std::optional<uint64_t> active, const RetentionPins& pins)
{
    if (published >= UINT64_MAX - 1 || !history || history > published + 1)
        throw Error(Fault::invalid, "Invalid checkpoint publication range");
    auto floor = history;
    for (const auto pin : {active, pins.backup, pins.dataset})
    {
        if (!pin) continue;
        if (!*pin || *pin > published + 1) throw Error(Fault::invalid, "Invalid checkpoint retention pin");
        floor = (std::min)(floor, *pin);
    }
    return {published, floor};
}

void save_checkpoint(const std::filesystem::path& data, Checkpoint value, const std::function<void()>& before_publish)
{
    const auto destination = checkpoint_path(data);
    if (present(destination)) verify_checkpoint(destination, value.published);
    auto temporary = destination;
    static std::atomic_uint64_t sequence{0};
    temporary += ".pending." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(GetTickCount64())
        + "." + std::to_string(++sequence);

    Page content{};
    std::ranges::copy(checkpoint_magic, content.begin());
    put<uint32_t>(content, 8, 1);
    put<uint64_t>(content, 16, value.published);
    put<uint64_t>(content, 24, value.retain_from);
    seal(content);
    {
        File file(temporary, File::Mode::create_new);
        file.write(0, content);
        file.flush();
    }
    if (before_publish) before_publish();
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw Error(Fault::io, "Could not publish checkpoint metadata");
}
}
