#include "server.hpp"
#include "storage_admin.hpp"

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <openssl/rand.h>

#include <csignal>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>

namespace sdp::server
{
namespace
{
using asio::ip::tcp;
using asio::ssl::stream;
using wire::Code;
using wire::Json;
using namespace std::chrono_literals;

constexpr auto product_version = "0.1.0";

std::string read_tls_file(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::string contents(256 * 1024 + 1, '\0');
    if (!file.read(contents.data(), static_cast<std::streamsize>(contents.size())) && (!file.eof() || file.bad()))
        throw StartupError("Cannot read TLS material");
    const auto size = static_cast<size_t>(file.gcount());
    if (size == 0 || size == contents.size()) throw StartupError("Invalid TLS material size");
    contents.resize(size);
    return contents;
}

void configure_tls(asio::ssl::context& context, const Config& config)
{
    try
    {
        if (SSL_CTX_set_min_proto_version(context.native_handle(), TLS1_2_VERSION) != 1)
            throw StartupError("Cannot set the minimum TLS version");
        context.set_options(asio::ssl::context::no_compression);
        context.set_password_callback([](size_t, asio::ssl::context::password_purpose) { return std::string{}; });
        SSL_CTX_set_session_cache_mode(context.native_handle(), SSL_SESS_CACHE_OFF);
        SSL_CTX_set_options(context.native_handle(), SSL_OP_NO_TICKET);

        const auto certificate = read_tls_file(config.certificate);
        auto key = read_tls_file(config.private_key);
        try
        {
            context.use_certificate_chain(asio::buffer(certificate));
            context.use_private_key(asio::buffer(key), asio::ssl::context::pem);
        }
        catch (...)
        {
            OPENSSL_cleanse(key.data(), key.size());
            throw;
        }
        OPENSSL_cleanse(key.data(), key.size());
        if (SSL_CTX_check_private_key(context.native_handle()) != 1)
            throw StartupError("TLS private key does not match the certificate");
    }
    catch (const StartupError&) { throw; }
    catch (...) { throw StartupError("Invalid TLS certificate or key"); }
}

std::string session_id()
{
    std::array<unsigned char, 16> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) throw wire::Error(Code::internal);
    constexpr std::string_view hex = "0123456789abcdef";
    std::string id;
    id.reserve(random.size() * 2);
    for (const auto byte : random)
    {
        id += hex[byte >> 4];
        id += hex[byte & 15];
    }
    return id;
}

struct SecretPayload
{
    std::string bytes;
    ~SecretPayload() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
};

void check_io(const asio::error_code& error)
{
    if (!error) return;
    if (error == asio::error::operation_aborted) throw wire::Error(Code::timeout);
    throw wire::Error(Code::connection_lost);
}

class Session
{
public:
    Session(tcp::socket socket, asio::ssl::context& context, const Config& config, const Admin& admin)
        : stream_(std::move(socket), context), config_(config), admin_(admin) {}

    void stop() noexcept
    {
        stopping_ = true;
        asio::error_code ignored;
        stream_.lowest_layer().cancel(ignored);
    }

    asio::awaitable<void> run()
    {
        asio::error_code error;
        co_await stream_.async_handshake(asio::ssl::stream_base::server, token(error));
        if (error)
        {
            if (!stopping_) log_event(config_, false, "tls_handshake_failed");
            co_return;
        }
        id_ = session_id();
        log_event(config_, false, "session_open", id_);

        std::optional<Code> protocol_error;
        try
        {
            while (!stopping_ && !auth_failed_)
            {
                current_id_ = 0;
                wire::FrameHeader header{};
                co_await asio::async_read(stream_, asio::buffer(header), token(error));
                check_io(error);
                SecretPayload payload{std::string(wire::payload_size(header), '\0')};
                co_await asio::async_read(stream_, asio::buffer(payload.bytes), token(error));
                check_io(error);
                auto packet = wire::parse_payload(payload.bytes);
                auto reply = dispatch(packet);
                co_await send(reply);
            }
        }
        catch (const wire::Error& failure)
        {
            const auto code = failure.code();
            if (!stopping_ && (code == Code::malformed || code == Code::too_large || code == Code::incompatible))
                protocol_error = code;
            else if (!stopping_ && code == Code::timeout)
                log_event(config_, false, "session_timeout", id_);
        }
        if (protocol_error)
        {
            log_event(config_, false, "protocol_rejected", id_);
            try { co_await send(wire::failure(current_id_, *protocol_error, correlation())); }
            catch (const wire::Error&) {}
        }

        co_await stream_.async_shutdown(asio::cancel_after(500ms,
            asio::redirect_error(asio::use_awaitable, error)));
        log_event(config_, false, "session_closed", id_);
    }

private:
    auto token(asio::error_code& error) const
        -> decltype(asio::cancel_after(1ms, asio::redirect_error(asio::use_awaitable, error)))
    {
        return asio::cancel_after(config_.timeout, asio::redirect_error(asio::use_awaitable, error));
    }

    asio::awaitable<void> send(Json reply)
    {
        const auto frame = wire::encode_frame(reply);
        asio::error_code error;
        co_await asio::async_write(stream_, asio::buffer(frame), token(error));
        check_io(error);
    }

    std::string correlation() const
    {
        return id_ + "-" + std::to_string(current_id_);
    }

    Json dispatch(Json& packet)
    {
        current_id_ = wire::request_id(packet);
        wire::check_request(packet);
        if (current_id_ <= last_id_) throw wire::Error(Code::malformed);
        last_id_ = current_id_;

        const auto& operation = wire::string_field(packet, "op");
        auto& body = packet.at("body");
        if (operation == "hello" && !introduced_)
        {
            introduced_ = true;
            return wire::success(current_id_, {{"session_id", id_},
                {"product_version", product_version}, {"protocol_version", wire::version}});
        }
        if (!introduced_ || operation == "hello" || (authenticated_ && operation == "auth"))
            return wire::failure(current_id_, Code::invalid_state, correlation());

        if (operation == "auth")
        {
            const auto& username = wire::string_field(body, "username");
            wire::string_field(body, "password");
            auto& password = body.at("password").get_ref<std::string&>();
            try
            {
                authenticated_ = admin_.verify(username, password);
            }
            catch (...)
            {
                OPENSSL_cleanse(password.data(), password.size());
                throw;
            }
            OPENSSL_cleanse(password.data(), password.size());
            auth_failed_ = !authenticated_;
            log_event(config_, true, authenticated_ ? "authentication_passed" : "authentication_failed", correlation());
            if (authenticated_)
                return wire::success(current_id_, {{"authenticated", true}});
            return wire::failure(current_id_, Code::auth_failed, correlation());
        }
        if (!authenticated_) return wire::failure(current_id_, Code::permission_denied, correlation());
        if (operation == "capabilities")
            return wire::success(current_id_, {{"features", wire::capabilities(true)}});
        if (operation == "health")
            return wire::success(current_id_, {{"status", "ready"}, {"product_version", product_version}});
        return wire::failure(current_id_, Code::unsupported, correlation());
    }

    stream<tcp::socket> stream_;
    const Config& config_;
    const Admin& admin_;
    std::string id_;
    uint32_t current_id_{};
    uint32_t last_id_{};
    bool introduced_{};
    bool authenticated_{};
    bool auth_failed_{};
    bool stopping_{};
};

class Service
{
public:
    Service(const Config& config, const Admin& admin, asio::ssl::context tls)
        : config_(config), admin_(admin), tls_(std::move(tls)), signals_(io_, SIGINT, SIGTERM)
    {
        const tcp::endpoint endpoint(asio::ip::make_address(config.bind), config.port);
        asio::error_code error;
        acceptor_.open(endpoint.protocol(), error);
        if (!error)
        {
            // Prevent a second Windows listener from reusing the same port.
            const int exclusive = 1;
            if (setsockopt(acceptor_.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0)
                throw StartupError("Cannot set exclusive port ownership");
            acceptor_.bind(endpoint, error);
        }
        if (!error) acceptor_.listen(static_cast<int>(config.max_connections), error);
        if (error) throw StartupError("Bind/listen failed. Check port availability");
    }

    int run()
    {
        signals_.async_wait([this](const asio::error_code& error, int) {
            if (!error) stop();
        });
        accept();
        std::cout << "LISTENING " << config_.bind << ' ' << acceptor_.local_endpoint().port() << std::endl;
        log_event(config_, false, "listening");
        io_.run();
        log_event(config_, false, "stopped");
        return failed_ ? 1 : 0;
    }

private:
    void accept()
    {
        acceptor_.async_accept([this](const asio::error_code& error, tcp::socket socket) {
            if (stopping_) return;
            if (error)
            {
                failed_ = true;
                log_event(config_, false, "accept_failed");
                stop();
                return;
            }
            if (sessions_.size() < config_.max_connections)
            {
                auto session = std::make_shared<Session>(std::move(socket), tls_, config_, admin_);
                const auto key = ++next_session_;
                sessions_.emplace(key, session);
                asio::co_spawn(io_, session->run(), [this, session, key](std::exception_ptr failure) {
                    if (failure) log_event(config_, false, "session_error");
                    sessions_.erase(key);
                });
            }
            accept();
        });
    }

    void stop()
    {
        stopping_ = true;
        asio::error_code ignored;
        acceptor_.close(ignored);
        signals_.cancel(ignored);
        for (const auto& [key, session] : sessions_) session->stop();
    }

    const Config& config_;
    const Admin& admin_;
    asio::io_context io_{1};
    asio::ssl::context tls_;
    tcp::acceptor acceptor_{io_};
    asio::signal_set signals_;
    std::map<uint64_t, std::shared_ptr<Session>> sessions_;
    uint64_t next_session_{};
    bool stopping_{};
    bool failed_{};
};
}

int run(const Config& config)
{
    asio::ssl::context tls(asio::ssl::context::tls_server);
    configure_tls(tls, config);
    const DataLock lock(config);
    check_storage_on_start(config);
    const auto admin = load_admin(config);
    Service service(config, admin, std::move(tls));
    return service.run();
}
}
