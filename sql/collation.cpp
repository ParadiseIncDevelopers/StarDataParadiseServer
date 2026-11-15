#include "collation.hpp"
#include "diagnostic.hpp"

#include <windows.h>

#include <algorithm>
#include <limits>

namespace sdp::sql
{
namespace
{
constexpr auto turkish_locale = L"tr-TR";
constexpr DWORD turkish_flags = LINGUISTIC_IGNORECASE | NORM_LINGUISTIC_CASING;

bool ascii_equal(std::string_view left, std::string_view right)
{
    return std::ranges::equal(left, right, [](char a, char b) {
        return (a >= 'A' && a <= 'Z' ? a + ('a' - 'A') : a) == b;
    });
}

std::wstring wide_text(std::string_view input)
{
    if (input.empty()) return {};
    if (input.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
        throw Error(Code::limit, {}, "Collation input exceeds Windows NLS limits");
    const auto count = static_cast<int>(input.size());
    const auto required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), count, nullptr, 0);
    if (!required) throw Error(Code::type, {}, "Collation input is not valid UTF-8");
    std::wstring result(required, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), count, result.data(), required))
        throw Error(Code::type, {}, "Collation input conversion failed");
    return result;
}

void append_hex(std::string& target, unsigned long value, unsigned width)
{
    constexpr std::string_view hex = "0123456789abcdef";
    while (width)
    {
        --width;
        target += hex[(value >> (width * 4)) & 15];
    }
}
}

std::string_view canonical_collation(std::string_view name)
{
    if (name.empty() || ascii_equal(name, "binary")) return "binary";
    if (ascii_equal(name, "turkish") || ascii_equal(name, "tr-tr")) return "tr-TR";
    throw Error(Code::unsupported, {}, "Unsupported collation. Use BINARY or TURKISH");
}

std::string collation_version(std::string_view name)
{
    if (canonical_collation(name) == "binary") return "utf8-binary-v1";

    NLSVERSIONINFOEX info{};
    info.dwNLSVersionInfoSize = sizeof(info);
    if (!GetNLSVersionEx(COMPARE_STRING, turkish_locale, &info))
        throw Error(Code::unsupported, {}, "Windows Turkish collation is unavailable");
    std::string result = "nls-turkish-case-v1:";
    append_hex(result, info.dwNLSVersion, 8);
    result += ':';
    append_hex(result, info.dwEffectiveId, 8);
    result += ':';
    const auto& guid = info.guidCustomVersion;
    append_hex(result, guid.Data1, 8);
    append_hex(result, guid.Data2, 4);
    append_hex(result, guid.Data3, 4);
    for (const auto byte : guid.Data4) append_hex(result, byte, 2);
    return result;
}

int compare_collated(std::string_view left, std::string_view right, std::string_view name)
{
    if (canonical_collation(name) == "binary")
    {
        const auto compared = left.compare(right);
        return (compared > 0) - (compared < 0);
    }
    const auto a = wide_text(left);
    const auto b = wide_text(right);
    const auto result = CompareStringEx(turkish_locale, turkish_flags,
        a.c_str(), a.empty() ? -1 : static_cast<int>(a.size()),
        b.c_str(), b.empty() ? -1 : static_cast<int>(b.size()), nullptr, nullptr, 0);
    if (!result) throw Error(Code::type, {}, "Windows Turkish collation comparison failed");
    return result - CSTR_EQUAL;
}
}
