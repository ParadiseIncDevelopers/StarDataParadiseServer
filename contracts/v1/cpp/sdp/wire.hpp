#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sdp::wire
{
using Json = nlohmann::json;
inline constexpr uint32_t version = 1;
inline constexpr uint32_t max_payload = 1024 * 1024;
inline constexpr int max_depth = 32;
using FrameHeader = std::array<uint8_t, 4>;

enum class Code : int32_t
{
    malformed = 1001,
    too_large = 1002,
    incompatible = 1003,
    unsupported = 1004,
    auth_failed = 1005,
    permission_denied = 1006,
    module_unavailable = 1007,
    timeout = 1008,
    cancelled = 1009,
    connection_lost = 1010,
    tls_failed = 1011,
    invalid_argument = 1012,
    invalid_state = 1013,
    resource_limit = 1014,
    internal = 1015,
    out_of_memory = 1016
};

struct ErrorText
{
    const char* state;
    const char* message;
};

inline constexpr ErrorText describe(Code code)
{
    switch (code)
    {
    case Code::malformed: return {"08P01", "Malformed protocol message"};
    case Code::too_large: return {"54000", "Protocol frame exceeds the size limit"};
    case Code::incompatible: return {"0A000", "Incompatible protocol version"};
    case Code::unsupported: return {"0A000", "Feature unavailable"};
    case Code::auth_failed: return {"28000", "Authentication failed"};
    case Code::permission_denied: return {"42501", "Permission denied"};
    case Code::module_unavailable: return {"55000", "Module unavailable"};
    case Code::timeout: return {"HYT00", "Operation timed out"};
    case Code::cancelled: return {"57014", "Operation cancelled"};
    case Code::connection_lost: return {"08006", "Connection closed"};
    case Code::tls_failed: return {"08001", "TLS connection failed"};
    case Code::invalid_argument: return {"22023", "Invalid argument"};
    case Code::invalid_state: return {"55000", "Operation is invalid in the current state"};
    case Code::resource_limit: return {"53000", "A resource limit was reached"};
    case Code::internal: return {"XX000", "Internal error"};
    case Code::out_of_memory: return {"HY001", "Out of memory"};
    }
    return {"XX000", "Unknown error"};
}

class Error : public std::runtime_error
{
public:
    explicit Error(Code code) : std::runtime_error(describe(code).message), code_(code) {}
    Code code() const noexcept { return code_; }

private:
    Code code_;
};

inline FrameHeader header_for(size_t size)
{
    if (size == 0) throw Error(Code::malformed);
    if (size > max_payload) throw Error(Code::too_large);

    return {static_cast<uint8_t>(size >> 24), static_cast<uint8_t>(size >> 16),
            static_cast<uint8_t>(size >> 8), static_cast<uint8_t>(size)};
}

inline uint32_t payload_size(const FrameHeader& header)
{
    const auto size = (uint32_t{header[0]} << 24) | (uint32_t{header[1]} << 16)
                      | (uint32_t{header[2]} << 8) | uint32_t{header[3]};
    if (size == 0) throw Error(Code::malformed);
    if (size > max_payload) throw Error(Code::too_large);
    return size;
}

inline Json parse_payload(std::string_view data)
{
    header_for(data.size());
    if (data.starts_with("\xef\xbb\xbf") || data.find('\0') != data.npos)
        throw Error(Code::malformed);

    std::vector<std::set<std::string>> keys;
    const auto validate = [&keys](int depth, Json::parse_event_t event, Json& item)
    {
        using Event = Json::parse_event_t;
        if ((event == Event::object_start || event == Event::array_start) && depth >= max_depth)
            throw Error(Code::malformed);

        if (event == Event::object_start) keys.emplace_back();
        if (event == Event::key && !keys.back().insert(item.get_ref<const std::string&>()).second)
            throw Error(Code::malformed);
        if (event == Event::object_end) keys.pop_back();
        return true;
    };

    auto value = Json::parse(data.begin(), data.end(), validate, false);
    if (!value.is_object()) throw Error(Code::malformed);
    return value;
}

inline std::string serialize(const Json& value)
{
    if (!value.is_object()) throw Error(Code::malformed);
    try
    {
        auto bytes = value.dump();
        header_for(bytes.size());
        return bytes;
    }
    catch (const Json::exception&)
    {
        throw Error(Code::malformed);
    }
}

inline std::string encode_frame(const Json& value)
{
    const auto bytes = serialize(value);
    const auto header = header_for(bytes.size());
    std::string frame(header.begin(), header.end());
    frame += bytes;
    return frame;
}

inline Json decode_frame(std::string_view frame)
{
    if (frame.size() < FrameHeader{}.size()) throw Error(Code::malformed);
    FrameHeader header{};
    for (size_t i = 0; i < header.size(); ++i) header[i] = static_cast<uint8_t>(frame[i]);
    const auto length = payload_size(header);
    if (frame.size() - header.size() != length) throw Error(Code::malformed);
    return parse_payload(frame.substr(header.size(), length));
}

inline uint32_t request_id(const Json& message)
{
    const auto id = message.find("id");
    if (id == message.end() || !id->is_number_unsigned()
        || id->get<uint64_t>() > (std::numeric_limits<uint32_t>::max)())
        throw Error(Code::malformed);
    return id->get<uint32_t>();
}

inline void check_version(const Json& message)
{
    const auto value = message.find("v");
    if (value == message.end() || !value->is_number_integer()) throw Error(Code::malformed);
    if (*value != version) throw Error(Code::incompatible);
}

inline const std::string& string_field(const Json& object, const char* key)
{
    const auto value = object.find(key);
    if (value == object.end() || !value->is_string()) throw Error(Code::malformed);
    return value->get_ref<const std::string&>();
}

inline const Json& object_field(const Json& object, const char* key)
{
    const auto value = object.find(key);
    if (value == object.end() || !value->is_object()) throw Error(Code::malformed);
    return *value;
}

inline void check_request(const Json& message)
{
    check_version(message);
    if (request_id(message) == 0 || string_field(message, "op").empty())
        throw Error(Code::malformed);
    object_field(message, "body");
}

inline Json request(uint32_t id, std::string_view op, Json body = Json::object())
{
    return {{"v", version}, {"id", id}, {"op", op}, {"body", std::move(body)}};
}

inline Json success(uint32_t id, Json body)
{
    return {{"v", version}, {"id", id}, {"ok", true}, {"body", std::move(body)}};
}

inline Json failure(uint32_t id, Code code, Json correlation = nullptr)
{
    const auto info = describe(code);
    return {{"v", version}, {"id", id}, {"ok", false}, {"error", {
        {"code", static_cast<int32_t>(code)}, {"sqlstate", info.state}, {"message", info.message},
        {"line", nullptr}, {"column", nullptr}, {"retryable", false}, {"unknown_outcome", false},
        {"correlation_id", std::move(correlation)}
    }}};
}

inline Json capabilities(bool service_available)
{
    return {{"tls", service_available}, {"auth", service_available}, {"health", service_available},
        {"metadata", false}, {"sql", false}, {"transactions", false}, {"server_cancel", false},
        {"jobs", false}, {"admin", false}, {"history", false}, {"decision", false}, {"ml", false}};
}
}
