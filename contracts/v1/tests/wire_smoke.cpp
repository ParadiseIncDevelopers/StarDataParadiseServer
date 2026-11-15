#include <sdp/wire.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>

using sdp::wire::Code;
using sdp::wire::Error;
using sdp::wire::Json;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class Operation>
void expect_error(Code code, Operation operation)
{
    try { operation(); }
    catch (const Error& error)
    {
        require(error.code() == code, "unexpected protocol error");
        return;
    }
    throw std::runtime_error("expected a protocol error");
}

std::string load(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    require(file.is_open(), "cannot open golden fixture");
    return {std::istreambuf_iterator<char>(file), {}};
}

int main(int argc, char** argv)
{
    namespace wire = sdp::wire;
    try
    {
        require(argc == 3, "pass values and capabilities fixtures");
        const auto message = wire::parse_payload(load(argv[1]));
        using namespace std::string_literals;
        const auto frame = wire::encode_frame(message);
        const auto roundtrip = wire::decode_frame(frame);
        require(message == roundtrip, "roundtrip changed values");
        require(wire::encode_frame(roundtrip) == frame, "frame encoding changed");
        const auto& values = roundtrip.at("body").at("values");
        require(values.at(0) == Json({{"t","decimal"},{"unscaled","-123456789012345678901234567890123400"},{"scale",4}}),
                "decimal coefficient or scale changed");
        require(values.at(1).at("t") == "null" && values.at(2).at("v") == "", "NULL and empty differ");
        require(values.at(3).at("v") == "İstanbul, Iğdır, çığ, şüphe, ölü, ı", "UTF-8 text changed");
        require(values.at(4).at("hex") == "00017f80feff", "binary bytes changed");
        require(values.at(5).at("v") == "-9223372036854775808" &&
                values.at(6).at("v") == "9223372036854775807", "i64 endpoints changed");
        require(values.at(7).at("v") == "a\0b"s, "embedded NUL changed");
        require(values.at(8).at("nanos") == 999999999 && values.at(8).at("zone") == "Europe/Istanbul",
                "timestamp context changed");

        expect_error(Code::malformed, []{ wire::decode_frame("abc"); });
        expect_error(Code::malformed, []{ wire::decode_frame("\0\0\0\0"s); });
        expect_error(Code::malformed, []{ wire::decode_frame("\0\0\0\x04{}"s); });
        expect_error(Code::malformed, []{ wire::parse_payload("{\"x\":]"); });
        expect_error(Code::malformed, []{ wire::parse_payload("{\"x\":1,\"x\":2}"); });
        expect_error(Code::malformed, []{ wire::parse_payload("{\"x\":\"\xc0\x80\"}"); });
        expect_error(Code::malformed, []{ wire::parse_payload("{}\0extra"s); });
        expect_error(Code::malformed, []{ wire::parse_payload("{\"x\":" + std::string(33, '[') + "0" + std::string(33, ']') + "}"); });
        expect_error(Code::too_large, []{ wire::payload_size({0, 0x10, 0, 1}); });
        expect_error(Code::too_large, []{ wire::parse_payload(std::string(wire::max_payload + 1, ' ')); });
        expect_error(Code::incompatible, []{ wire::check_request(Json{{"v", 2}}); });
        expect_error(Code::malformed, []{ wire::check_request(Json{{"v", 1.0}}); });
        expect_error(Code::malformed, []{ wire::check_request(wire::parse_payload(R"({"v":1,"id":4294967296,"op":"hello","body":{}})")); });
        wire::check_request(wire::parse_payload(wire::serialize(wire::request(1, "hello"))));

        const auto capabilities = wire::parse_payload(load(argv[2]));
        require(capabilities.at("features") == wire::capabilities(false), "capability fixture mismatch");
        require(std::all_of(capabilities.at("features").begin(), capabilities.at("features").end(),
                    [](const auto& feature) { return feature == false; }), "premature supported feature");
        const auto error = wire::failure(5, Code::unsupported).at("error");
        require(error.at("code") == 1004 && error.at("sqlstate") == "0A000"
            && error.at("retryable") == false && error.at("unknown_outcome") == false, "error contract changed");
        std::cout << "v1 roundtrip, rejected frames, versions and capability checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
