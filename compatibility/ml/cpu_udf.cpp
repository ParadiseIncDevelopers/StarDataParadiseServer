#include "cpu_compute.hpp"

#include <mysql/udf_registration_types.h>

#include <cstring>
#include <new>

#ifdef _WIN32
#define SDP_UDF_EXPORT extern "C" __declspec(dllexport)
#else
#define SDP_UDF_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace
{
constexpr unsigned long max_request_bytes = 2 * 1024 * 1024;
constexpr unsigned long max_result_bytes = 1024 * 1024;
}

SDP_UDF_EXPORT bool sdp_model_compute_init(UDF_INIT* init, UDF_ARGS* args, char* message) noexcept
{
    if (args->arg_count != 1 || args->arg_type[0] != STRING_RESULT)
    {
        std::strcpy(message, "sdp_model_compute expects one JSON string");
        return true;
    }
    auto* result = new (std::nothrow) std::string;
    if (!result)
    {
        std::strcpy(message, "sdp_model_compute could not allocate its result buffer");
        return true;
    }
    init->ptr = reinterpret_cast<char*>(result);
    init->maybe_null = true;
    init->const_item = false;
    init->max_length = max_result_bytes;
    return false;
}

SDP_UDF_EXPORT void sdp_model_compute_deinit(UDF_INIT* init) noexcept
{
    delete reinterpret_cast<std::string*>(init->ptr);
}

SDP_UDF_EXPORT char* sdp_model_compute(UDF_INIT* init, UDF_ARGS* args, char*, unsigned long* length,
    unsigned char* is_null, unsigned char* error) noexcept
{
    *error = 0;
    *is_null = 1;
    *length = 0;
    const auto size = args->lengths[0];
    if (!args->args[0] || size == 0 || size > max_request_bytes) return nullptr;

    try
    {
        using sdp::wire::Json;
        const std::string_view text(args->args[0], size);
        const auto request = Json::parse(text, [](int depth, Json::parse_event_t event, Json&) {
            if (depth >= sdp::wire::max_depth &&
                (event == Json::parse_event_t::object_start || event == Json::parse_event_t::array_start))
                throw sdp::wire::Error(sdp::wire::Code::malformed);
            return true;
        });
        auto& result = *reinterpret_cast<std::string*>(init->ptr);
        result = sdp::mysql_model::compute_sql(request).dump();
        if (result.size() <= max_result_bytes)
        {
            *length = static_cast<unsigned long>(result.size());
            *is_null = 0;
            return result.data();
        }
    }
    catch (...)
    {
        // SQL routines translate invalid input to their own fixed diagnostics.
    }
    return nullptr;
}
