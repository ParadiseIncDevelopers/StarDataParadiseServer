#include "storage.hpp"

#include <windows.h>
#include <iostream>
#include <limits>

using namespace sdp::disk;

void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class Operation>
void rejects(Fault reason, Operation operation)
{
    try { operation(); }
    catch (const Error& error)
    {
        if (reason == error.fault()) return;
        throw;
    }
    throw std::runtime_error("Storage accepted an invalid input or failed I/O");
}

Row sample()
{
    Bytes binary(15000);
    for (size_t i = 0; i < binary.size(); ++i) binary[i] = static_cast<Byte>(i * 29);
    return {17, {INT64_MIN, Decimal{"-123456789012345678901234567890123400", 4},
        std::string("Türkçe İstanbul Iğdır"), std::monostate{}, std::string{}, std::string("A\0B", 3), binary}};
}

template<class Update>
void corrupt(const std::filesystem::path& file, PageId id, Update edit, bool update_checksum)
{
    File disk(file, File::Mode::read_write);
    Page page{};
    disk.read(uint64_t{id} * page_size, page);
    edit(page);
    if (update_checksum) seal(page);
    disk.write(uint64_t{id} * page_size, page);
    disk.flush();
}

void round_trip(const std::filesystem::path& root)
{
    const auto file = root / "fixture-v1.sdp";
    Store::initialize(file);
    const auto expected = sample();
    RowId id, deleted;
    PageId pages{};
    {
        Store store(file, false, 2);
        Editor write(store);
        id = write.insert(expected);
        deleted = write.insert(Row{1, {Bytes(9000, 0x7e)}});
        store.apply(write.prepare(1));
        check(store.inspect().rows == 2 && store.cached_pages() <= 2, "cache or row count mismatch");
        pages = store.header().pages;
    }
    {
        Store store(file, false, 1);
        Editor write(store);
        check(write.get(id) == expected, "round trip changed the row");
        write.erase(deleted);
        const auto next = write.insert(Row{1, {Bytes(8000, 0x22)}});
        check(next != deleted && next != id, "deleted row identity was reused");
        rejects(Fault::missing, [&] { write.get(deleted); });
        store.apply(write.prepare(2));
        check(store.header().pages == pages && store.inspect().rows == 2, "overflow pages were not reused");
        check(store.cached_pages() <= 1, "one-page cache grew");
    }
    {
        Store store(file, true, 2);
        Editor read(store);
        check(read.get(id) == expected, "reopen changed row values");
        rejects(Fault::io, [&] { Store second(file); });
        rejects(Fault::io, [&] { store.apply(read.prepare(3)); });
    }
    rejects(Fault::io, [&] { Store::initialize(file); });
    std::cout << "Binary, UTF-8, decimal, NULL, BLOB, stable row IDs and free-page reuse passed\n";
}

void malformed(const std::filesystem::path& root)
{
    const auto original = root / "fixture-v1.sdp";
    const auto copy = [&](const char* name) {
        auto file = root / name;
        std::filesystem::copy_file(original, file);
        return file;
    };

    const auto header = copy("header.sdp");
    corrupt(header, 0, [](Page& bytes) { put<uint32_t>(bytes, 8, 99); }, true);
    rejects(Fault::format, [&] { Store invalid(header, true); });

    const auto crc = copy("checksum.sdp");
    corrupt(crc, 1, [](Page& bytes) { bytes[3000] ^= 1; }, false);
    rejects(Fault::corrupt, [&] { Store store(crc, true); store.inspect(); });

    const auto overflow = copy("overflow.sdp");
    PageId first{};
    {
        File disk(overflow, File::Mode::read_only);
        Page page{};
        disk.read(page_size, page);
        first = load_le<PageId>(page, prefix + 8);
    }
    check(first != 0, "fixture has no overflow chain");
    corrupt(overflow, first, [&](Page& bytes) { put<PageId>(bytes, 24, first); }, true);
    rejects(Fault::corrupt, [&] { Store store(overflow, true); store.inspect(); });

    const auto offset = copy("offset.sdp");
    corrupt(offset, 1, [](Page& bytes) { put<uint16_t>(bytes, prefix, UINT16_MAX); }, true);
    rejects(Fault::corrupt, [&] { Store store(offset, true); store.inspect(); });

    auto row = encode(sample());
    row.pop_back();
    rejects(Fault::corrupt, [&] { decode(row); });
    rejects(Fault::invalid, [&] { encode(Row{1, {std::string("\xc0\x80")}}); });
    rejects(Fault::invalid, [&] { encode(Row{1, {Decimal{"-0", 5}}}); });
    std::cout << "Invalid version, checksum, overflow loop, offset, UTF-8 and short row were rejected\n";
}

void io_errors(const std::filesystem::path& root)
{
    auto short_read = root / "short.sdp";
    Store::initialize(short_read);
    std::filesystem::resize_file(short_read, 128);
    rejects(Fault::io, [&] { Store store(short_read, true); });

    const auto read_only = root / "read-only.sdp";
    Store::initialize(read_only);
    check(SetFileAttributesW(read_only.c_str(), FILE_ATTRIBUTE_READONLY), "cannot mark fixture read-only");
    try { rejects(Fault::io, [&] { Store store(read_only); }); }
    catch (...)
    {
        SetFileAttributesW(read_only.c_str(), FILE_ATTRIBUTE_NORMAL);
        throw;
    }
    check(SetFileAttributesW(read_only.c_str(), FILE_ATTRIBUTE_NORMAL), "cannot restore fixture attributes");

    const auto short_write = root / "write.sdp";
    Store::initialize(short_write);
    Store store(short_write, false, 2, [](Io io, size_t size) {
        return io == Io::write ? size - 1 : size;
    });
    Editor edit(store);
    const auto id = edit.insert(Row{1, {int64_t{42}}});
    rejects(Fault::io, [&] { store.apply(edit.prepare(1)); });
    check(store.header().lsn == 0, "failed write advanced current LSN");
    rejects(Fault::io, [&] { store.read(static_cast<PageId>(id >> 32)); });

    const auto bounded = root / "budget.sdp";
    Store::initialize(bounded);
    Store budget(bounded);
    Editor constrained(budget, 1);
    rejects(Fault::limit, [&] { constrained.insert(sample()); });
    check(constrained.snapshot().changed.empty(), "failed edit left dirty pages");
    std::cout << "Exclusive file ownership, bounded cache/edit, short read/write and OS denial passed\n";
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc != 2) return 2;
        auto root = std::filesystem::path(argv[1]) / ("storage-" + std::to_string(GetCurrentProcessId()) + "-"
            + std::to_string(GetTickCount64()));
        if (!std::filesystem::create_directories(root)) return 2;
        round_trip(root);
        malformed(root);
        io_errors(root);
        std::cout << "Storage format 1 fixture validated\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
