#include "wal.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <utility>

namespace sdp::disk
{
namespace
{
constexpr size_t segment_size = 64;
constexpr size_t record_header = 32;
constexpr size_t marker_size = 24;
constexpr size_t frame_limit = 4 * 1024 * 1024;
constexpr uint32_t record_magic = 0x52504453u; // SDPR
constexpr uint32_t marker_magic = 0x43504453u; // SDPC
constexpr std::array<Byte, 8> magic{'S', 'D', 'P', 'W', 'A', 'L', '1', 0};

void checked_append(Bytes& target, std::span<const Byte> source)
{
    if (source.size() > frame_limit - target.size()) throw Error(Fault::limit, "WAL record exceeds limit");
    target.insert(target.end(), source.begin(), source.end());
}

Bytes record(const Frame& frame)
{
    const auto& batch = frame.batch;
    if (!batch.header.lsn || batch.header.pages > page_limit || batch.pages.size() > 256 ||
        frame.changes.empty() || frame.changes.size() > change_count_limit)
        throw Error(Fault::invalid, "Invalid WAL batch");
    Bytes bytes(record_header);
    for (const auto& [id, page] : batch.pages)
    {
        if (!id || id >= batch.header.pages || load_le<uint64_t>(page, 12) != batch.header.lsn)
            throw Error(Fault::invalid, "Invalid WAL page image");
        validate_page(page, id, batch.header.lsn);
        std::array<Byte, sizeof(PageId)> identity{};
        put<PageId>(identity, 0, id);
        checked_append(bytes, identity);
        checked_append(bytes, page);
    }
    size_t journal_bytes{};
    for (const auto& change : frame.changes)
    {
        if ((!change.before.size() && !change.after.size()) || change.before.size() > row_limit ||
            change.after.size() > row_limit || change.before.size() + change.after.size() > change_budget - journal_bytes)
            throw Error(Fault::limit, "Invalid WAL change-set size");
        journal_bytes += change.before.size() + change.after.size();
        std::array<Byte, 20> identity{};
        put<RowId>(identity, 0, change.id);
        put<uint32_t>(identity, 8, static_cast<uint32_t>(change.kind));
        put<uint32_t>(identity, 12, static_cast<uint32_t>(change.before.size()));
        put<uint32_t>(identity, 16, static_cast<uint32_t>(change.after.size()));
        checked_append(bytes, identity);
        checked_append(bytes, change.before);
        checked_append(bytes, change.after);
    }
    put<uint32_t>(bytes, 0, record_magic);
    put<uint32_t>(bytes, 4, static_cast<uint32_t>(bytes.size()));
    put<uint64_t>(bytes, 8, batch.header.lsn);
    put<uint32_t>(bytes, 16, static_cast<uint32_t>(batch.pages.size()));
    put<uint32_t>(bytes, 20, static_cast<uint32_t>(frame.changes.size()));
    put<PageId>(bytes, 24, batch.header.pages);
    put<uint32_t>(bytes, 28, checksum(std::span(bytes).first(28)));
    return bytes;
}

std::span<const Byte> take(std::span<const Byte>& source, size_t count)
{
    if (count > source.size()) throw Error(Fault::corrupt, "Invalid WAL payload extent");
    const auto field = source.first(count);
    source = source.subspan(count);
    return field;
}

Frame read_frame(std::span<const Byte> data)
{
    const auto header = take(data, record_header);
    const auto page_count = load_le<uint32_t>(header, 16);
    const auto change_count = load_le<uint32_t>(header, 20);
    Frame frame;
    auto& batch = frame.batch;
    batch.header = {load_le<PageId>(header, 24), load_le<uint64_t>(header, 8)};
    if (!batch.header.pages || batch.header.pages > page_limit || page_count == 0 || page_count > 256 ||
        !change_count || change_count > change_count_limit)
        throw Error(Fault::corrupt, "WAL batch counts are out of range");
    PageId previous{};
    for (uint32_t index = 0; index < page_count; ++index)
    {
        const auto id = load_le<PageId>(take(data, 4), 0);
        const auto bytes = take(data, page_size);
        if (id <= previous || id >= batch.header.pages || load_le<uint64_t>(bytes, 12) != batch.header.lsn)
            throw Error(Fault::corrupt, "Invalid WAL page ordering or LSN");
        Page page{};
        std::ranges::copy(bytes, page.begin());
        validate_page(page, id, batch.header.lsn);
        batch.pages.emplace(id, std::move(page));
        previous = id;
    }
    size_t journal_bytes{};
    frame.changes.reserve(change_count);
    for (uint32_t index = 0; index < change_count; ++index)
    {
        const auto identity = take(data, 20);
        const auto id = load_le<RowId>(identity, 0);
        const auto kind = static_cast<ChangeKind>(load_le<uint32_t>(identity, 8));
        const auto before_size = load_le<uint32_t>(identity, 12);
        const auto after_size = load_le<uint32_t>(identity, 16);
        const auto row_page = static_cast<PageId>(id >> 32);
        const auto slot = static_cast<uint32_t>(id);
        if (!row_page || row_page >= batch.header.pages || !slot || slot > (page_end - prefix) / 16 ||
            !batch.pages.contains(row_page) || (kind != ChangeKind::row && kind != ChangeKind::catalog) ||
            (!before_size && !after_size) || before_size > row_limit || after_size > row_limit ||
            size_t{before_size} + after_size > change_budget - journal_bytes)
            throw Error(Fault::corrupt, "WAL change-set fields are invalid");
        journal_bytes += before_size + after_size;
        const auto before = take(data, before_size);
        const auto after = take(data, after_size);
        if (!before.empty()) decode(before);
        if (!after.empty()) decode(after);
        frame.changes.push_back({id, kind, Bytes(before.begin(), before.end()), Bytes(after.begin(), after.end())});
    }
    if (!data.empty()) throw Error(Fault::corrupt, "Extra bytes in WAL payload");
    return frame;
}

uint64_t read_segment(File& file)
{
    if (file.size() < segment_size || file.size() > wal_quota)
        throw Error(Fault::corrupt, "Invalid WAL segment length");
    std::array<Byte, segment_size> header{};
    file.read(0, header);
    if (!std::ranges::equal(magic, std::span(header).first(magic.size())) ||
        load_le<uint32_t>(header, 8) != 1 || load_le<uint32_t>(header, 12) != segment_size)
        throw Error(Fault::format, "Unrecognized WAL format");
    if (load_le<uint32_t>(header, segment_size - 4) != checksum(std::span(header).first(segment_size - 4)) ||
        !std::ranges::all_of(std::span(header).subspan(24, segment_size - 28), [](Byte byte) { return byte == 0; }))
        throw Error(Fault::corrupt, "WAL segment header checksum or fields are invalid");
    const auto first = load_le<uint64_t>(header, 16);
    if (!first || first == UINT64_MAX) throw Error(Fault::corrupt, "Invalid segment start LSN");
    return first;
}

std::array<Byte, marker_size> marker(std::span<const Byte> bytes)
{
    std::array<Byte, marker_size> result{};
    put<uint32_t>(result, 0, marker_magic);
    put<uint32_t>(result, 4, static_cast<uint32_t>(bytes.size()));
    put<uint64_t>(result, 8, load_le<uint64_t>(bytes, 8));
    put<uint32_t>(result, 16, checksum(bytes));
    put<uint32_t>(result, 20, checksum(std::span(result).first(20)));
    return result;
}
}

std::filesystem::path segment_path(const std::filesystem::path& data, uint64_t first)
{
    auto path = data;
    const auto digits = std::to_string(first);
    path += ".wal." + std::string(20 - digits.size(), '0') + digits;
    return path;
}

std::unique_ptr<File> open_segment(const std::filesystem::path& path, uint64_t first, bool create, IoHook hook)
{
    const auto mode = create ? File::Mode::create_new : File::Mode::read_write;
    auto file = std::make_unique<File>(path, mode, std::move(hook));
    if (create)
    {
        std::array<Byte, segment_size> header{};
        std::ranges::copy(magic, header.begin());
        put<uint32_t>(header, 8, 1);
        put<uint32_t>(header, 12, segment_size);
        put<uint64_t>(header, 16, first);
        put<uint32_t>(header, segment_size - 4, checksum(std::span(header).first(segment_size - 4)));
        file->write(0, header);
        file->flush();
    }
    if (read_segment(*file) != first) throw Error(Fault::corrupt, "WAL segment start LSN does not match its name");
    return file;
}

Scan scan(File& file, const VisitFrame& visit)
{
    const auto first = read_segment(file);
    Scan tail{first, first - 1, segment_size};
    const auto end = file.size();
    while (tail.valid_end < end)
    {
        const auto available = end - tail.valid_end;
        if (available < record_header) { tail.incomplete_tail = true; break; }

        std::array<Byte, record_header> header{};
        file.read(tail.valid_end, header);
        const auto size = load_le<uint32_t>(header, 4);
        if (load_le<uint32_t>(header, 0) != record_magic || size < record_header || size > frame_limit ||
            load_le<uint32_t>(header, 28) != checksum(std::span(header).first(28)))
            throw Error(Fault::corrupt, "WAL record header is corrupt");
        if (available < uint64_t{size} + marker_size) { tail.incomplete_tail = true; break; }

        Bytes bytes(size);
        file.read(tail.valid_end, bytes);
        std::array<Byte, marker_size> end_marker{};
        file.read(tail.valid_end + size, end_marker);
        if (end_marker != marker(bytes)) throw Error(Fault::corrupt, "WAL commit marker or record checksum invalid");
        auto frame = read_frame(bytes);
        if (tail.last >= UINT64_MAX - 1 || frame.batch.header.lsn != tail.last + 1)
            throw Error(Fault::corrupt, "WAL LSN gap");
        tail.last = frame.batch.header.lsn;
        tail.valid_end += size + marker_size;
        ++tail.commits;
        if (visit) visit(std::move(frame));
    }
    return tail;
}

void append_commit(File& file, const Frame& frame, const std::function<void()>& after_body, uint64_t segment_budget)
{
    const auto bytes = record(frame);
    const auto commit = marker(bytes);
    const auto start = file.size();
    if (start > segment_budget || bytes.size() + commit.size() > segment_budget - start)
        throw Error(Fault::limit, "WAL or History quota reached");

    try
    {
        file.write(start, bytes);
        if (after_body) after_body();
    }
    catch (...)
    {
        try { file.resize(start); file.flush(); }
        catch (...) { throw Error(Fault::uncertain, "WAL cleanup failed"); }
        throw Error(Fault::io, "WAL body was not committed");
    }

    try
    {
        file.write(start + bytes.size(), commit);
        file.flush();
    }
    catch (...)
    {
        throw Error(Fault::uncertain, "WAL commit durability could not be confirmed");
    }
}
}
