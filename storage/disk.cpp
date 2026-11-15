#include "storage.hpp"

#include <windows.h>

#include <algorithm>
#include <bit>
#include <limits>
#include <utility>

namespace sdp::disk
{
namespace
{
constexpr std::array<Byte, 8> database_magic{'S', 'D', 'P', 'S', 'T', '0', '1', 0};
constexpr uint32_t data_magic = 0x47504453; // SDPG
constexpr uint32_t version = 1;
constexpr size_t max_fields = 128;

void append(Bytes& target, std::span<const Byte> source)
{
    if (source.size() > row_limit - target.size()) throw Error(Fault::limit, "Row exceeds the byte limit");
    target.insert(target.end(), source.begin(), source.end());
}

template<class T>
void append_number(Bytes& target, T value)
{
    if (sizeof(T) > row_limit - target.size()) throw Error(Fault::limit, "Row exceeds the byte limit");
    const auto offset = target.size();
    target.resize(offset + sizeof(T));
    put<T>(target, offset, value);
}

std::span<const Byte> view(std::string_view value)
{
    return {reinterpret_cast<const Byte*>(value.data()), value.size()};
}

bool decimal_valid(std::string_view value, uint8_t scale)
{
    if (value.empty() || scale > 38) return false;
    const bool negative = value.front() == '-';
    if (negative) value.remove_prefix(1);
    if (value.empty() || value.size() > 38 || (value.front() == '0' && (value.size() != 1 || negative)))
        return false;
    return value.find_first_not_of("0123456789") == value.npos;
}

bool utf8_valid(std::string_view text)
{
    return text.empty() || (text.size() <= row_limit &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0) > 0);
}

class Reader
{
public:
    explicit Reader(std::span<const Byte> bytes) : bytes_(bytes) {}
    std::span<const Byte> take(size_t count)
    {
        if (count > bytes_.size()) throw Error(Fault::corrupt, "Row field is truncated");
        const auto result = bytes_.first(count);
        bytes_ = bytes_.subspan(count);
        return result;
    }
    template<class T> T number() { return load_le<T>(take(sizeof(T)), 0); }
    std::string string(size_t count)
    {
        const auto data = take(count);
        return {reinterpret_cast<const char*>(data.data()), data.size()};
    }
    bool empty() const noexcept { return bytes_.empty(); }

private:
    std::span<const Byte> bytes_;
};
}

uint32_t checksum(std::span<const Byte> bytes) noexcept
{
    static constexpr auto table = [] {
        std::array<uint32_t, 256> result{};
        for (uint32_t index = 0; index < result.size(); ++index)
        {
            uint32_t entry = index;
            for (int bit = 0; bit < 8; ++bit)
                entry = (entry >> 1) ^ ((entry & 1) ? 0xedb88320u : 0u);
            result[index] = entry;
        }
        return result;
    }();
    uint32_t crc = 0xffffffffu;
    for (const auto byte : bytes) crc = table[(crc ^ byte) & 0xffu] ^ (crc >> 8);
    return ~crc;
}

void seal(Page& page)
{
    put<uint32_t>(page, page_end, checksum(std::span(page).first(page_end)));
}

bool sealed(const Page& page)
{
    return checksum(std::span(page).first(page_end)) == load_le<uint32_t>(page, page_end);
}

Header read_header(const Page& bytes)
{
    if (!std::ranges::equal(database_magic, std::span(bytes).first(database_magic.size())) ||
        load_le<uint32_t>(bytes, 8) != version || load_le<uint32_t>(bytes, 12) != page_size)
        throw Error(Fault::format, "Unknown storage format or page size");
    if (!sealed(bytes)) throw Error(Fault::corrupt, "Storage header checksum mismatch");
    Header header{load_le<PageId>(bytes, 16), load_le<uint64_t>(bytes, 24)};
    if (!header.pages || header.pages > page_limit ||
        !std::ranges::all_of(std::span(bytes).subspan(20, 4), [](Byte byte) { return byte == 0; }) ||
        !std::ranges::all_of(std::span(bytes).subspan(32, page_end - 32), [](Byte byte) { return byte == 0; }))
        throw Error(Fault::corrupt, "Invalid storage header fields");
    return header;
}

Page make_header(Header header)
{
    Page bytes{};
    std::ranges::copy(database_magic, bytes.begin());
    put<uint32_t>(bytes, 8, version);
    put<uint32_t>(bytes, 12, page_size);
    put<PageId>(bytes, 16, header.pages);
    put<uint64_t>(bytes, 24, header.lsn);
    seal(bytes);
    return bytes;
}

Kind validate_page(const Page& page, PageId id, uint64_t max_lsn)
{
    if (!sealed(page) || load_le<uint32_t>(page, 0) != data_magic || load_le<PageId>(page, 8) != id ||
        load_le<uint16_t>(page, 6) != prefix || load_le<uint64_t>(page, 12) > max_lsn ||
        load_le<uint32_t>(page, 28) != 0)
        throw Error(Fault::corrupt, "Page header, identity, LSN or checksum invalid");
    const auto type = static_cast<Kind>(load_le<uint16_t>(page, 4));
    if (type != Kind::heap && type != Kind::overflow && type != Kind::free)
        throw Error(Fault::corrupt, "Unknown page type");
    return type;
}

File::File(const std::filesystem::path& path, Mode mode, IoHook hook) : hook_(std::move(hook))
{
    const bool read_only = mode == Mode::read_only;
    const auto file = CreateFileW(path.c_str(), GENERIC_READ | (read_only ? 0 : GENERIC_WRITE),
        0, nullptr, mode == Mode::create_new ? CREATE_NEW : OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw Error(Fault::io, "Could not open the storage file exclusively");
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &info, sizeof(info)) ||
        (info.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
    {
        CloseHandle(file);
        throw Error(Fault::io, "Storage must use a regular file");
    }
    handle_ = file;
}

File::~File()
{
    if (handle_) CloseHandle(handle_);
}

uint64_t File::size() const
{
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(handle_, &length)) throw Error(Fault::io, "Could not inspect storage file size");
    return static_cast<uint64_t>(length.QuadPart);
}

void File::seek(uint64_t offset)
{
    if (offset > static_cast<uint64_t>(INT64_MAX)) throw Error(Fault::limit, "File offset exceeds the limit");
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<int64_t>(offset);
    if (!SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN)) throw Error(Fault::io, "Could not seek storage file");
}

size_t File::requested(Io io, size_t bytes) const
{
    const auto length = hook_ ? hook_(io, bytes) : bytes;
    if (length > bytes || length > (std::numeric_limits<DWORD>::max)())
        throw Error(Fault::limit, "I/O request exceeds the limit");
    return length;
}

void File::read(uint64_t offset, std::span<Byte> bytes)
{
    seek(offset);
    DWORD count{};
    if (!ReadFile(handle_, bytes.data(), static_cast<DWORD>(requested(Io::read, bytes.size())), &count, nullptr) ||
        count != bytes.size()) throw Error(Fault::io, "Storage read failed or was short");
}

void File::write(uint64_t offset, std::span<const Byte> bytes)
{
    seek(offset);
    DWORD count{};
    if (!WriteFile(handle_, bytes.data(), static_cast<DWORD>(requested(Io::write, bytes.size())), &count, nullptr) ||
        count != bytes.size()) throw Error(Fault::io, "Storage write failed or was short");
}

void File::resize(uint64_t length)
{
    seek(length);
    if (!SetEndOfFile(handle_)) throw Error(Fault::io, "Could not resize storage file");
}

void File::flush()
{
    requested(Io::flush, 0);
    if (!FlushFileBuffers(handle_)) throw Error(Fault::io, "Storage flush failed");
}

Bytes encode(const Row& row)
{
    const auto count = row.values.size();
    if (!row.schema || !count || count > max_fields) throw Error(Fault::invalid, "Invalid row schema or field count");
    const auto bitmap_size = (count + 7) / 8;
    Bytes out(8 + bitmap_size);
    put<uint16_t>(out, 0, static_cast<uint16_t>(count));
    put<uint16_t>(out, 2, static_cast<uint16_t>(bitmap_size));
    put<uint32_t>(out, 4, row.schema);

    for (size_t index = 0; index < count; ++index)
    {
        const auto& field = row.values[index];
        if (std::holds_alternative<std::monostate>(field))
        {
            out[8 + index / 8] |= static_cast<Byte>(1u << (index % 8));
            continue;
        }
        append_number(out, static_cast<Byte>(field.index()));
        if (const auto* integer = std::get_if<int64_t>(&field))
            append_number(out, std::bit_cast<uint64_t>(*integer));
        else if (const auto* decimal = std::get_if<Decimal>(&field))
        {
            if (!decimal_valid(decimal->unscaled, decimal->scale)) throw Error(Fault::invalid, "Invalid decimal");
            append_number(out, decimal->scale);
            append_number(out, static_cast<Byte>(decimal->unscaled.size()));
            append(out, view(decimal->unscaled));
        }
        else if (const auto* text = std::get_if<std::string>(&field))
        {
            if (!utf8_valid(*text)) throw Error(Fault::invalid, "Invalid UTF-8 or text too large");
            append_number(out, static_cast<uint32_t>(text->size()));
            append(out, view(*text));
        }
        else
        {
            const auto& blob = std::get<Bytes>(field);
            if (blob.size() > row_limit) throw Error(Fault::limit, "Binary field too large");
            append_number(out, static_cast<uint32_t>(blob.size()));
            append(out, blob);
        }
    }
    return out;
}

Row decode(std::span<const Byte> bytes)
{
    if (bytes.size() > row_limit) throw Error(Fault::corrupt, "Stored row exceeds the limit");
    Reader input(bytes);
    const auto count = input.number<uint16_t>();
    const auto nulls = input.number<uint16_t>();
    const auto schema = input.number<uint32_t>();
    if (!count || count > max_fields || nulls != (count + 7) / 8 || !schema)
        throw Error(Fault::corrupt, "Stored row header invalid");
    const auto bitmap = input.take(nulls);
    if (const unsigned last_bits = count % 8; last_bits && (bitmap.back() >> last_bits))
        throw Error(Fault::corrupt, "Stored null bitmap invalid");

    Row row{schema, {}};
    row.values.reserve(count);
    for (size_t index = 0; index < count; ++index)
    {
        if (bitmap[index / 8] & (1u << (index % 8)))
        {
            row.values.emplace_back(std::monostate{});
            continue;
        }
        switch (input.number<Byte>())
        {
        case 1: row.values.emplace_back(std::bit_cast<int64_t>(input.number<uint64_t>())); break;
        case 2:
        {
            const auto scale = input.number<Byte>();
            auto text = input.string(input.number<Byte>());
            if (!decimal_valid(text, scale)) throw Error(Fault::corrupt, "Stored decimal invalid");
            row.values.emplace_back(Decimal{std::move(text), scale});
            break;
        }
        case 3:
        {
            auto text = input.string(input.number<uint32_t>());
            if (!utf8_valid(text)) throw Error(Fault::corrupt, "Stored UTF-8 invalid");
            row.values.emplace_back(std::move(text));
            break;
        }
        case 4:
        {
            const auto value = input.take(input.number<uint32_t>());
            row.values.emplace_back(Bytes(value.begin(), value.end()));
            break;
        }
        default: throw Error(Fault::corrupt, "Unknown stored field type");
        }
    }
    if (!input.empty()) throw Error(Fault::corrupt, "Unexpected bytes after stored row");
    return row;
}
}
