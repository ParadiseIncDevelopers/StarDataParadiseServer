#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace sdp::disk
{
using Byte = uint8_t;
using Bytes = std::vector<Byte>;
using PageId = uint32_t;
using RowId = uint64_t;
constexpr size_t page_size = 4096;
constexpr size_t page_end = page_size - 4;
constexpr size_t prefix = 32;
constexpr size_t row_limit = 512 * 1024;
constexpr PageId page_limit = 65536;
using Page = std::array<Byte, page_size>;
using PageMap = std::map<PageId, Page>;

enum class Fault { io, format, corrupt, limit, missing, invalid, busy, cancelled, uncertain };
class Error : public std::runtime_error
{
public:
    Error(Fault fault, const char* message) : std::runtime_error(message), fault_(fault) {}
    Fault fault() const noexcept { return fault_; }
private:
    Fault fault_;
};

template<class T>
T load_le(std::span<const Byte> source, size_t offset)
{
    static_assert(std::is_unsigned_v<T>);
    if (offset > source.size() || source.size() - offset < sizeof(T))
        throw Error(Fault::corrupt, "Truncated field");
    uint64_t value{};
    for (size_t i = 0; i < sizeof(T); ++i) value |= uint64_t{source[offset + i]} << (i * 8);
    return static_cast<T>(value);
}

template<class T>
void put(std::span<Byte> target, size_t offset, T value)
{
    static_assert(std::is_unsigned_v<T>);
    if (offset > target.size() || target.size() - offset < sizeof(T))
        throw Error(Fault::limit, "Field exceeds output");
    for (size_t i = 0; i < sizeof(T); ++i) target[offset + i] = static_cast<Byte>(value >> (i * 8));
}

uint32_t checksum(std::span<const Byte> bytes) noexcept;
void seal(Page& page);
bool sealed(const Page& page);

enum class Io { read, write, flush };
using IoHook = std::function<size_t(Io, size_t)>;

class File
{
public:
    enum class Mode { read_only, read_write, create_new };
    File(const std::filesystem::path& path, Mode mode, IoHook hook = {});
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    uint64_t size() const;
    void read(uint64_t offset, std::span<Byte> bytes);
    void write(uint64_t offset, std::span<const Byte> bytes);
    void resize(uint64_t size);
    void flush();

private:
    void seek(uint64_t offset);
    size_t requested(Io io, size_t bytes) const;

    void* handle_{};
    IoHook hook_;
};

struct Decimal
{
    std::string unscaled;
    uint8_t scale{};
    bool operator==(const Decimal&) const = default;
};
using Value = std::variant<std::monostate, int64_t, Decimal, std::string, Bytes>;
struct Row
{
    uint32_t schema = 1;
    std::vector<Value> values;
    bool operator==(const Row&) const = default;
};
Bytes encode(const Row& row);
Row decode(std::span<const Byte> bytes);

enum class Kind : uint16_t { free = 1, heap = 2, overflow = 3 };

struct Header
{
    PageId pages = 1;
    uint64_t lsn{};
};
Header read_header(const Page& bytes);
Page make_header(Header header);
Kind validate_page(const Page& page, PageId id, uint64_t max_lsn);

struct Batch
{
    Header header;
    PageMap pages;
};

struct Integrity
{
    PageId heap_pages{};
    PageId overflow_pages{};
    PageId free_pages{};
    uint64_t rows{};
};

using PageReader = std::function<Page(PageId)>;
using RowVisitor = std::function<void(RowId, const Row&)>;
using ScanRow = std::function<bool(RowId, const Row&)>;
Integrity inspect_pages(const Header& header, const PageReader& read, const RowVisitor& visit = {},
    std::span<const RowId> index_candidates = {});

enum class Publication { page_written, file_flushed };
using PublishHook = std::function<void(Publication)>;

class Store
{
public:
    static void initialize(const std::filesystem::path& path);
    Store(const std::filesystem::path& path, bool read_only = false, size_t cache_pages = 16, IoHook hook = {});
    const Header& header() const noexcept { return header_; }
    Page read(PageId id);
    size_t cached_pages() const noexcept { return cache_.size(); }
    void apply(const Batch& batch, const PublishHook& hook = {});
    Integrity inspect(std::span<const RowId> index_candidates = {});

private:
    friend class Transaction;
    File file_;
    Header header_;
    size_t capacity_;
    std::list<std::pair<PageId, Page>> cache_;
    bool usable_ = true;
    bool read_only_;
};

class Editor
{
public:
    struct Snapshot
    {
        PageId count;
        PageMap changed;
    };

    explicit Editor(Store& store, size_t writable_pages = 256)
        : store_(store), dirty_{store.header().pages, {}}, limit_(writable_pages) {}
    Row get(RowId id);
    void visit(const RowVisitor& visitor, std::optional<uint32_t> schema = {});
    // False indicates that the visitor stopped before scanning all rows.
    bool scan(RowId after, const ScanRow& visitor, std::optional<uint32_t> schema = {},
        const std::function<void()>& check = {});
    RowId insert(const Row& row);
    void update(RowId id, const Row& row);
    void erase(RowId id);
    void restore_row(RowId id, const Row& row);
    Snapshot snapshot() const { return dirty_; }
    void restore(Snapshot snapshot) { dirty_ = std::move(snapshot); }
    Batch prepare(uint64_t lsn) const;

private:
    enum class RowEdit { append, replace, restore };
    Page read(PageId id);
    void write(PageId id, Page image);
    PageId allocate(Kind kind);
    void replace(PageId id, uint16_t slot, std::span<const Byte> bytes, RowEdit edit);
    Bytes record(PageId id, uint16_t slot, std::vector<PageId>* references = nullptr);

    Store& store_;
    Snapshot dirty_;
    size_t limit_;
};
}
