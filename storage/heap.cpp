#include "storage.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace sdp::disk
{
namespace
{
constexpr uint32_t data_magic = 0x47504453;
constexpr size_t slot_size = 16;
constexpr size_t inline_limit = 512;
constexpr size_t overflow_capacity = page_end - prefix;

struct Slot
{
    uint16_t offset{};
    uint16_t length{};
    uint32_t total{};
    PageId first{};
};

Page blank(PageId id, Kind kind)
{
    Page page{};
    put<uint32_t>(page, 0, data_magic);
    put<uint16_t>(page, 4, static_cast<uint16_t>(kind));
    put<uint16_t>(page, 6, prefix);
    put<PageId>(page, 8, id);
    if (kind == Kind::heap) put<uint16_t>(page, 22, page_end);
    return page;
}

Kind kind_of(const Page& page) { return static_cast<Kind>(load_le<uint16_t>(page, 4)); }
uint16_t slot_count(const Page& page) { return load_le<uint16_t>(page, 20); }
size_t slot_offset(uint16_t index) { return prefix + size_t{index} * slot_size; }

Slot read_slot(const Page& page, uint16_t index)
{
    const auto offset = slot_offset(index);
    return {load_le<uint16_t>(page, offset), load_le<uint16_t>(page, offset + 2),
        load_le<uint32_t>(page, offset + 4), load_le<PageId>(page, offset + 8)};
}

void write_slot(Page& page, uint16_t index, Slot slot)
{
    if (!slot.total) return;
    const auto offset = slot_offset(index);
    put<uint16_t>(page, offset, slot.offset);
    put<uint16_t>(page, offset + 2, slot.length);
    put<uint32_t>(page, offset + 4, slot.total);
    put<PageId>(page, offset + 8, slot.first);
}

uint16_t check_heap(const Page& page)
{
    const auto slots = slot_count(page);
    const auto directory_end = slot_offset(slots);
    const auto head = load_le<uint16_t>(page, 22);
    if (directory_end > page_end || head < directory_end || head > page_end || load_le<PageId>(page, 24))
        throw Error(Fault::corrupt, "Invalid heap directory");
    std::vector<std::pair<size_t, size_t>> limits;
    limits.reserve(slots);
    for (uint16_t index = 0; index < slots; ++index)
    {
        const auto slot = read_slot(page, index);
        if (load_le<uint32_t>(page, slot_offset(index) + 12) || slot.total > row_limit ||
            slot.length > inline_limit || slot.length > slot.total ||
            (slot.first != 0) != (slot.total > slot.length) ||
            (slot.length == 0 ? slot.offset != 0 :
                size_t{slot.offset} < head || size_t{slot.offset} + slot.length > page_end))
            throw Error(Fault::corrupt, "Invalid row slot offset or length");
        if (slot.length) limits.emplace_back(slot.offset, size_t{slot.offset} + slot.length);
    }
    std::ranges::sort(limits);
    size_t previous = head;
    for (const auto& [start, end] : limits)
    {
        if (start != previous) throw Error(Fault::corrupt, "Overlapping or noncompact row slots");
        previous = end;
    }
    if (previous != page_end) throw Error(Fault::corrupt, "Heap payload boundaries are invalid");
    return slots;
}

Bytes read_record(const Page& page, Slot slot, const PageReader& read, std::vector<PageId>* references)
{
    Bytes result;
    result.reserve(slot.total);
    if (slot.length) result.insert(result.end(), page.begin() + slot.offset, page.begin() + slot.offset + slot.length);
    std::set<PageId> seen;
    for (auto next = slot.first; next;)
    {
        if (!seen.insert(next).second) throw Error(Fault::corrupt, "Overflow chain cycle");
        const auto chunk = read(next);
        const auto count = load_le<uint16_t>(chunk, 22);
        if (kind_of(chunk) != Kind::overflow || slot_count(chunk) || !count ||
            count > overflow_capacity || count > slot.total - result.size())
            throw Error(Fault::corrupt, "Invalid overflow chunk");
        result.insert(result.end(), chunk.begin() + prefix, chunk.begin() + prefix + count);
        if (references) references->push_back(next);
        next = load_le<PageId>(chunk, 24);
    }
    if (result.size() != slot.total) throw Error(Fault::corrupt, "Incomplete overflow chain");
    return result;
}

std::pair<PageId, uint16_t> split(RowId id)
{
    const auto slot = static_cast<uint32_t>(id);
    if (!slot || slot > UINT16_MAX || !(id >> 32)) throw Error(Fault::missing, "Invalid row ID");
    return {static_cast<PageId>(id >> 32), static_cast<uint16_t>(slot - 1)};
}
}

void Store::initialize(const std::filesystem::path& path)
{
    File file(path, File::Mode::create_new);
    const auto image = make_header({});
    file.write(0, image);
    file.flush();
}

Store::Store(const std::filesystem::path& path, bool read_only, size_t cache_pages, IoHook hook)
    : file_(path, read_only ? File::Mode::read_only : File::Mode::read_write, std::move(hook)),
      capacity_(cache_pages), read_only_(read_only)
{
    if (capacity_ < 1 || capacity_ > 64) throw Error(Fault::invalid, "Cache capacity must be 1–64 pages");
    Page header{};
    file_.read(0, header);
    header_ = read_header(header);
    if (file_.size() != uint64_t{header_.pages} * page_size)
        throw Error(Fault::corrupt, "Storage length differs from the page count");
}

Page Store::read(PageId id)
{
    if (!usable_) throw Error(Fault::io, "Storage became unavailable after an I/O failure");
    if (!id || id >= header_.pages) throw Error(Fault::corrupt, "Page reference is outside the file");
    const auto found = std::ranges::find(cache_, id, &std::pair<PageId, Page>::first);
    if (found != cache_.end())
    {
        cache_.splice(cache_.begin(), cache_, found);
        return found->second;
    }
    Page page{};
    file_.read(uint64_t{id} * page_size, page);
    validate_page(page, id, header_.lsn);
    if (cache_.size() == capacity_) cache_.pop_back();
    cache_.emplace_front(id, page);
    return page;
}

void Store::apply(const Batch& batch, const PublishHook& hook)
{
    if (read_only_ || !usable_) throw Error(Fault::io, "Storage is not writable");
    if (batch.header.pages < header_.pages || batch.header.pages > page_limit || batch.header.lsn <= header_.lsn)
        throw Error(Fault::invalid, "Invalid storage publication");
    for (const auto& [id, page] : batch.pages)
    {
        if (!id || id >= batch.header.pages || load_le<uint64_t>(page, 12) != batch.header.lsn)
            throw Error(Fault::invalid, "Invalid dirty page identity or LSN");
        validate_page(page, id, batch.header.lsn);
    }
    for (auto id = header_.pages; id < batch.header.pages; ++id)
        if (!batch.pages.contains(id)) throw Error(Fault::invalid, "Missing appended page");

    try
    {
        for (const auto& [id, image] : batch.pages)
        {
            file_.write(uint64_t{id} * page_size, image);
            if (hook) hook(Publication::page_written);
        }
        file_.write(0, make_header(batch.header));
        file_.flush();
        if (hook) hook(Publication::file_flushed);
    }
    catch (...)
    {
        usable_ = false;
        cache_.clear();
        throw;
    }
    header_ = batch.header;
    cache_.clear();
}

Page Editor::read(PageId id)
{
    if (!id || id >= dirty_.count) throw Error(Fault::corrupt, "Page reference is out of range");
    const auto entry = dirty_.changed.find(id);
    return entry == dirty_.changed.end() ? store_.read(id) : entry->second;
}

void Editor::write(PageId id, Page image)
{
    if (dirty_.changed.size() == limit_ && !dirty_.changed.contains(id))
        throw Error(Fault::limit, "Dirty page budget reached");
    dirty_.changed.insert_or_assign(id, std::move(image));
}

PageId Editor::allocate(Kind kind)
{
    for (PageId id = 1; id < dirty_.count; ++id)
    {
        if (kind_of(read(id)) != Kind::free) continue;
        write(id, blank(id, kind));
        return id;
    }
    if (dirty_.count == page_limit) throw Error(Fault::limit, "Storage page limit reached");
    const auto id = dirty_.count;
    write(id, blank(id, kind));
    ++dirty_.count;
    return id;
}

Bytes Editor::record(PageId id, uint16_t index, std::vector<PageId>* references)
{
    if (id >= dirty_.count) throw Error(Fault::missing, "Row page does not exist");
    const auto page = read(id);
    if (kind_of(page) != Kind::heap || index >= check_heap(page)) throw Error(Fault::missing, "Row slot does not exist");
    const auto slot = read_slot(page, index);
    if (!slot.total) throw Error(Fault::missing, "Row was deleted");
    return read_record(page, slot, [this](PageId link) { return read(link); }, references);
}

void Editor::replace(PageId id, uint16_t index, std::span<const Byte> bytes, RowEdit edit)
{
    const auto source = read(id);
    const auto count = check_heap(source);
    const bool append = edit == RowEdit::append;
    if (append ? index != count : index >= count || (read_slot(source, index).total != 0) != (edit == RowEdit::replace))
        throw Error(Fault::missing, "Row slot does not exist");

    if (edit == RowEdit::replace)
    {
        std::vector<PageId> old_chain;
        record(id, index, &old_chain);
        for (const auto page : old_chain) write(page, blank(page, Kind::free));
    }
    const auto new_count = static_cast<uint16_t>(count + (append ? 1 : 0));
    if (slot_offset(new_count) > page_end) throw Error(Fault::limit, "Heap page has no slot space");
    Page output = blank(id, Kind::heap);
    size_t cursor = page_end;
    for (uint16_t i = 0; i < count; ++i)
    {
        if (i == index) continue;
        auto entry = read_slot(source, i);
        if (entry.length)
        {
            cursor -= entry.length;
            std::copy_n(source.begin() + entry.offset, entry.length, output.begin() + cursor);
            entry.offset = static_cast<uint16_t>(cursor);
        }
        write_slot(output, i, entry);
    }
    if (cursor < slot_offset(new_count)) throw Error(Fault::limit, "Heap page has no directory space");
    const auto inline_bytes = (std::min)({inline_limit, bytes.size(), cursor - slot_offset(new_count)});
    PageId first{};
    auto remaining = bytes.subspan(inline_bytes);
    while (!remaining.empty())
    {
        const auto length = (std::min)(remaining.size(), overflow_capacity);
        const auto page_id = allocate(Kind::overflow);
        auto image = blank(page_id, Kind::overflow);
        put<uint16_t>(image, 22, static_cast<uint16_t>(length));
        put<PageId>(image, 24, first);
        std::ranges::copy(remaining.last(length), image.begin() + prefix);
        write(page_id, std::move(image));
        first = page_id;
        remaining = remaining.first(remaining.size() - length);
    }
    cursor -= inline_bytes;
    std::ranges::copy(bytes.first(inline_bytes), output.begin() + cursor);
    write_slot(output, index, {static_cast<uint16_t>(inline_bytes ? cursor : 0),
        static_cast<uint16_t>(inline_bytes), static_cast<uint32_t>(bytes.size()), first});
    put<uint16_t>(output, 20, new_count);
    put<uint16_t>(output, 22, static_cast<uint16_t>(cursor));
    write(id, std::move(output));
}

Row Editor::get(RowId id)
{
    const auto [page, slot] = split(id);
    return decode(record(page, slot));
}

void Editor::visit(const RowVisitor& visitor, std::optional<uint32_t> schema)
{
    scan(0, [&](RowId id, const Row& row) { visitor(id, row); return true; }, schema);
}

bool Editor::scan(RowId after, const ScanRow& visitor, std::optional<uint32_t> schema,
    const std::function<void()>& check)
{
    const auto read_page = [this, &check](PageId link) {
        if (check) check();
        return read(link);
    };
    const auto first_page = (std::max)(PageId{1}, static_cast<PageId>(after >> 32));
    for (PageId id = first_page; id < dirty_.count; ++id)
    {
        if (check) check();
        const auto page = read(id);
        if (kind_of(page) != Kind::heap) continue;
        const auto count = check_heap(page);
        for (uint16_t index = 0; index < count; ++index)
        {
            const auto identity = (uint64_t{id} << 32) | (uint64_t{index} + 1);
            if (identity <= after) continue;
            const auto slot = read_slot(page, index);
            if (!slot.total) continue;
            if (schema && slot.length >= 8 && load_le<uint32_t>(page, size_t{slot.offset} + 4) != *schema) continue;
            if (check) check();
            const auto row = decode(read_record(page, slot, read_page, nullptr));
            if ((!schema || row.schema == *schema) && !visitor(identity, row)) return false;
        }
    }
    return true;
}

RowId Editor::insert(const Row& row)
{
    const auto bytes = encode(row);
    auto before = snapshot();
    try
    {
        for (PageId id = 1; id < dirty_.count; ++id)
        {
            const auto image = read(id);
            if (kind_of(image) != Kind::heap) continue;
            const auto slots = check_heap(image);
            if (slot_offset(static_cast<uint16_t>(slots + 1)) > load_le<uint16_t>(image, 22)) continue;
            replace(id, slots, bytes, RowEdit::append);
            return (uint64_t{id} << 32) | (uint64_t{slots} + 1);
        }
        const auto id = allocate(Kind::heap);
        replace(id, 0, bytes, RowEdit::append);
        return (uint64_t{id} << 32) | 1u;
    }
    catch (...) { restore(std::move(before)); throw; }
}

void Editor::update(RowId id, const Row& row)
{
    const auto bytes = encode(row);
    const auto [page, slot] = split(id);
    get(id);
    auto before = snapshot();
    try { replace(page, slot, bytes, RowEdit::replace); }
    catch (...) { restore(std::move(before)); throw; }
}

void Editor::erase(RowId id)
{
    const auto [page, slot] = split(id);
    get(id);
    auto before = snapshot();
    try { replace(page, slot, {}, RowEdit::replace); }
    catch (...) { restore(std::move(before)); throw; }
}

void Editor::restore_row(RowId id, const Row& row)
{
    const auto bytes = encode(row);
    const auto [page, slot] = split(id);
    if (page >= dirty_.count || kind_of(read(page)) != Kind::heap)
        throw Error(Fault::missing, "Deleted row page no longer exists");
    auto before = snapshot();
    try { replace(page, slot, bytes, RowEdit::restore); }
    catch (...) { restore(std::move(before)); throw; }
}

Batch Editor::prepare(uint64_t lsn) const
{
    if (lsn <= store_.header().lsn) throw Error(Fault::invalid, "LSN must advance");
    Batch batch{{dirty_.count, lsn}, dirty_.changed};
    for (auto& [id, image] : batch.pages)
    {
        put<uint64_t>(image, 12, lsn);
        seal(image);
    }
    return batch;
}

Integrity inspect_pages(const Header& header, const PageReader& read, const RowVisitor& visit,
    std::span<const RowId> index_candidates)
{
    Integrity result;
    std::set<PageId> actual_overflow, referenced;
    std::set<RowId> index_refs(index_candidates.begin(), index_candidates.end());
    for (PageId id = 1; id < header.pages; ++id)
    {
        const auto page = read(id);
        switch (validate_page(page, id, header.lsn))
        {
        case Kind::heap:
        {
            ++result.heap_pages;
            const auto count = check_heap(page);
            for (uint16_t index = 0; index < count; ++index)
            {
                const auto slot = read_slot(page, index);
                if (!slot.total) continue;
                std::vector<PageId> chain;
                const auto row = decode(read_record(page, slot, read, &chain));
                for (const auto link : chain)
                    if (!referenced.insert(link).second) throw Error(Fault::corrupt, "Overflow page is shared by rows");
                const auto identity = (uint64_t{id} << 32) | (uint64_t{index} + 1);
                index_refs.erase(identity);
                if (visit) visit(identity, row);
                ++result.rows;
            }
            break;
        }
        case Kind::overflow: ++result.overflow_pages; actual_overflow.insert(id); break;
        case Kind::free:
            if (!std::ranges::all_of(std::span(page).subspan(20, page_end - 20), [](Byte byte) { return byte == 0; }))
                throw Error(Fault::corrupt, "Invalid free page");
            ++result.free_pages;
            break;
        }
    }
    if (actual_overflow != referenced) throw Error(Fault::corrupt, "Orphan overflow page");
    if (!index_refs.empty()) throw Error(Fault::corrupt, "Index candidate points to a missing row");
    return result;
}

Integrity Store::inspect(std::span<const RowId> index_candidates)
{
    return inspect_pages(header_, [this](PageId id) { return read(id); }, {}, index_candidates);
}
}
