#include "server.hpp"
#include "storage_admin.hpp"
#include "operations.hpp"
#include "../backup/archive.hpp"
#include "../sql/session.hpp"
#include "../sql/debugger.hpp"
#include "../jobs/manager.hpp"

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <openssl/rand.h>

#include <algorithm>
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

struct WorkerReply
{
    Json value;
    std::exception_ptr error;
};

template<class Job>
asio::awaitable<Json> on_worker(asio::thread_pool& workers, Job job)
{
    asio::use_awaitable_t<> token;
    auto completed = co_await asio::async_initiate<decltype(token), void(WorkerReply)>(
        [&workers, job = std::move(job)](auto handler) mutable {
            auto executor = asio::get_associated_executor(handler);
            auto keeps_running = asio::make_work_guard(executor);
            asio::post(workers, [job = std::move(job), executor, keeps_running = std::move(keeps_running),
                handler = std::move(handler)]() mutable {
                WorkerReply result;
                try { result.value = job(); }
                catch (...) { result.error = std::current_exception(); }
                asio::post(executor, [handler = std::move(handler), keeps_running = std::move(keeps_running),
                    result = std::move(result)]() mutable { handler(std::move(result)); });
            });
        }, token);
    if (completed.error) std::rethrow_exception(completed.error);
    co_return std::move(completed.value);
}

class Session : public std::enable_shared_from_this<Session>
{
public:
    using Control = std::function<Json(Session&, std::string_view, const Json&)>;

    Session(tcp::socket socket, asio::ssl::context& context, const Config& config, const Admin& admin,
        disk::Engine* database, disk::Owner owner, sql::Security* security, jobs::Manager* jobs,
        asio::thread_pool& workers, Operations* operations, Control control)
        : stream_(std::move(socket), context), config_(config), admin_(admin), database_(database), owner_(owner),
          security_(security), jobs_(jobs), workers_(workers), operations_(operations), control_(std::move(control)) {}

    const std::string& id() const noexcept { return id_; }
    const std::optional<sql::Principal>& principal() const noexcept { return principal_; }
    std::string correlation_id() const { return correlation(); }
    uint32_t request_number() const noexcept { return current_id_; }
    uint64_t slow_queries() const noexcept { return slow_queries_; }
    bool idle() const noexcept { return !busy_.has_value() && sql_.has_value(); }
    void attach_debugger(std::shared_ptr<sql::Debugger> debugger)
    {
        if (sql_) sql_->attach_debugger(std::move(debugger));
    }

    Json summary() const
    {
        using Clock = std::chrono::steady_clock;
        const auto elapsed = busy_ ? std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - busy_->started).count() : 0;
        return {{"session_id", id_}, {"actor", principal_->name}, {"transaction_active", sql_->transaction_active()},
            {"request_id", busy_ ? Json(busy_->request) : Json(nullptr)},
            {"operation", busy_ ? Json(busy_->operation) : Json(nullptr)},
            {"running_ms", static_cast<uint64_t>(elapsed)}, {"lock_wait_ms", sql_->lock_wait_ms()}};
    }

    bool cancel_request(uint32_t request)
    {
        if (!busy_ || (request && busy_->request != request)) return false;
        return busy_->source.request_stop();
    }

    void stop() noexcept
    {
        stopping_ = true;
        cancel_request(0);
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
                auto reply = co_await dispatch(packet);
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
        return asio::cancel_after(operations_ ? operations_->timeout() : config_.timeout,
            asio::redirect_error(asio::use_awaitable, error));
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

    asio::awaitable<Json> dispatch(Json& packet)
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
            co_return wire::success(current_id_, {{"session_id", id_},
                {"product_version", product_version}, {"protocol_version", wire::version}});
        }
        if (!introduced_ || operation == "hello" || (authenticated_ && operation == "auth"))
            co_return wire::failure(current_id_, Code::invalid_state, correlation());

        if (operation == "auth")
        {
            const auto& username = wire::string_field(body, "username");
            wire::string_field(body, "password");
            auto& password = body.at("password").get_ref<std::string&>();
            try
            {
                if (security_)
                {
                    principal_ = security_->authenticate(username, password);
                    authenticated_ = principal_.has_value();
                }
                else authenticated_ = admin_.verify(username, password);
            }
            catch (...)
            {
                OPENSSL_cleanse(password.data(), password.size());
                throw;
            }
            OPENSSL_cleanse(password.data(), password.size());
            auth_failed_ = !authenticated_;
            log_event(config_, true, authenticated_ ? "authentication_passed" : "authentication_failed", correlation());
            log_diagnostic(config_, "audit", authenticated_ ? "authentication_passed" : "authentication_failed", 0, id_);
            if (authenticated_)
            {
                if (database_)
                    sql_.emplace(*database_, owner_, principal_->name, operations_ ? operations_->timeout() : config_.timeout,
                        std::function<int64_t()>{}, config_.temp, security_, *principal_, config_.routine_test_instance, jobs_);
                co_return wire::success(current_id_, {{"authenticated", true}});
            }
            co_return wire::failure(current_id_, Code::auth_failed, correlation());
        }
        if (!authenticated_) co_return wire::failure(current_id_, Code::permission_denied, correlation());
        if (operation == "capabilities")
        {
            auto features = wire::capabilities(true);
            features["sql_crud_v1"] = sql_.has_value();
            features["sql_security_v1"] = security_ != nullptr;
            features["plsql_ir_v1"] = sql_.has_value();
            features["routine_export_v1"] = security_ != nullptr && sql_.has_value();
            features["plsql_results_v1"] = sql_.has_value();
            features["plsql_debug_v1"] = security_ != nullptr;
            features["history_v1"] = security_ != nullptr;
            features["history_restore_v1"] = security_ != nullptr;
            features["decision_v1"] = security_ != nullptr && config_.decision_enabled;
            features["decision_explain_v1"] = security_ != nullptr && config_.decision_enabled;
            features["jobs_v1"] = jobs_ != nullptr;
            features["dataset_v1"] = jobs_ != nullptr;
            features["supervised_ml_v1"] = jobs_ != nullptr;
            features["unsupervised_ml_v1"] = jobs_ != nullptr;
            features["mlp_cpu_v1"] = jobs_ != nullptr && jobs_->mlp_enabled();
            features["model_checkpoints_v1"] = jobs_ != nullptr && jobs_->mlp_enabled();
            features["model_lifecycle_v1"] = jobs_ != nullptr;
            features["ml_gpu_v1"] = false;
            features["backup_v1"] = jobs_ != nullptr && jobs_->backup_enabled();
            features["pitr_v1"] = false;
            features["admin_v1"] = operations_ != nullptr;
            features["health_center_v1"] = operations_ != nullptr;
            co_return wire::success(current_id_, {{"features", std::move(features)}});
        }
        if (operation == "health")
            co_return wire::success(current_id_, {{"status", "ready"}, {"product_version", product_version}});
        if (sql_)
        {
            if (operation.starts_with("admin_") && !principal_->administrator)
                co_return wire::failure(current_id_, Code::permission_denied, correlation());
            if (operation == "session_list" || operation == "session_terminate" || operation == "query_cancel" ||
                operation == "admin_status" || operation.starts_with("debug_"))
            {
                try { co_return control_(*this, operation, body); }
                catch (const wire::Error& error) { co_return wire::failure(current_id_, error.code(), correlation()); }
            }
            busy_.emplace(Busy{current_id_, operation, std::chrono::steady_clock::now(), {}});
            struct Clear
            {
                std::optional<Busy>& busy;
                ~Clear() { busy.reset(); }
            } clear{busy_};
            if (sql_->debugging())
            {
                // Readiness consumes no bytes. An idle watcher from a completed request is ignored by request ID.
                stream_.lowest_layer().async_wait(tcp::socket::wait_read,
                    [self = weak_from_this(), request = current_id_](const asio::error_code& error) {
                        if (!error)
                            if (const auto session = self.lock()) session->cancel_request(request);
                    });
            }
            const auto stop = busy_->source.get_token();
            const auto started = std::chrono::steady_clock::now();
            auto answer = co_await on_worker(workers_, [&] {
                if (!operation.starts_with("admin_") || operation == "admin_maintenance_submit")
                    return sql_->dispatch(current_id_, operation, body, correlation(), stop);
                if (!operations_) return wire::failure(current_id_, Code::unsupported, correlation());
                try { return wire::success(current_id_, operations_->request(operation, body, stop)); }
                catch (const wire::Error& error) { return wire::failure(current_id_, error.code(), correlation()); }
            });
            const auto elapsed = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count());
            const bool ok = answer.value("ok", false);
            const auto code = ok ? 0u : answer.at("error").value("code", 0u);
            if (operation == "execute" || operation == "fetch")
            {
                if (elapsed >= 250) ++slow_queries_;
                log_diagnostic(config_, "query", ok ? "query_completed" : "query_failed", code, id_, elapsed);
            }
            else if (operation.starts_with("history_"))
                log_diagnostic(config_, "history", ok ? "request_completed" : "request_failed", code, id_, elapsed);
            else if (operation.starts_with("model_") || operation.starts_with("dataset_"))
                log_diagnostic(config_, "ml", ok ? "request_completed" : "request_failed", code, id_, elapsed);
            else if (operation.starts_with("decision_"))
                log_diagnostic(config_, "decision", ok ? "decision_evaluated" : "decision_denied", code, id_, elapsed);
            else if (operation.starts_with("admin_") || operation.starts_with("backup_"))
                log_diagnostic(config_, "audit", ok ? "request_completed" : "request_failed", code, id_, elapsed);
            if (!ok) log_diagnostic(config_, "error", "request_failed", code, id_, elapsed);
            co_return answer;
        }
        co_return wire::failure(current_id_, Code::unsupported, correlation());
    }

    stream<tcp::socket> stream_;
    const Config& config_;
    const Admin& admin_;
    std::string id_;
    uint32_t current_id_{};
    uint32_t last_id_{};
    struct Busy
    {
        uint32_t request;
        std::string operation;
        std::chrono::steady_clock::time_point started;
        std::stop_source source;
    };
    disk::Engine* database_;
    disk::Owner owner_;
    sql::Security* security_;
    jobs::Manager* jobs_;
    asio::thread_pool& workers_;
    Operations* operations_;
    uint64_t slow_queries_{};
    Control control_;
    std::optional<sql::Principal> principal_;
    std::optional<Busy> busy_;
    std::optional<sql::Session> sql_;
    bool introduced_{};
    bool authenticated_{};
    bool auth_failed_{};
    bool stopping_{};
};

class Service
{
public:
    Service(const Config& config, const Admin& admin, asio::ssl::context tls,
        disk::Engine* database, sql::Security* security, jobs::Manager* jobs)
        : config_(config), admin_(admin), database_(database), security_(security), jobs_(jobs),
          operations_(database && jobs ? std::make_unique<Operations>(config, *database, *jobs) : nullptr),
          tls_(std::move(tls)), signals_(io_, SIGINT, SIGTERM)
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

    int run(std::stop_token token, const std::function<void()>& ready)
    {
        if (token.stop_requested()) return 0;
        signals_.async_wait([this](const asio::error_code& error, int) {
            if (!error) stop();
        });
        accept();
        std::cout << "LISTENING " << config_.bind << ' ' << acceptor_.local_endpoint().port() << std::endl;
        log_event(config_, false, "listening");
        log_diagnostic(config_, "operational", "listening");
        if (ready) ready();
        if (token.stop_possible()) watch_stop(token);
        io_.run();
        log_event(config_, false, "stopped");
        log_diagnostic(config_, "operational", "stopped");
        return failed_ ? 1 : 0;
    }

private:
    struct DebugBinding
    {
        std::string controller;
        std::weak_ptr<Session> target;
        std::shared_ptr<sql::Debugger> debugger;
    };

    Json debug_operation(Session& caller, std::string_view op, const Json& body)
    {
        const auto& actor = *caller.principal();
        if (!security_->allowed(actor, "debug")) throw wire::Error(Code::permission_denied);
        if (op == "debug_open")
        {
            const auto& target_id = wire::string_field(body, "session_id");
            const auto target = std::ranges::find_if(sessions_, [&](const auto& entry) { return entry.second->id() == target_id; });
            if (target == sessions_.end() || !target->second->principal() ||
                (!actor.administrator && target->second->principal()->id != actor.id))
                throw wire::Error(Code::permission_denied);
            if (!target->second->idle() || target->second.get() == &caller)
                throw wire::Error(Code::invalid_state);
            for (const auto& [id, binding] : debuggers_)
                if (binding.target.lock() == target->second) throw wire::Error(Code::invalid_state);
            if (debuggers_.size() >= config_.max_connections) throw wire::Error(Code::resource_limit);

            const auto revision = body.find("revision");
            if (revision == body.end() || !revision->is_number_unsigned() || !revision->get<uint64_t>() ||
                revision->get<uint64_t>() > UINT32_MAX) throw wire::Error(Code::invalid_argument);
            const auto& spelling = wire::string_field(body, "routine");
            if (spelling.empty() || spelling.size() > 386 || spelling.find('\0') != spelling.npos)
                throw wire::Error(Code::invalid_argument);
            sql::Name name;
            std::string_view remaining = spelling;
            for (;;)
            {
                const auto dot = remaining.find('.');
                const auto part = remaining.substr(0, dot);
                if (part.empty() || part.size() > 128 || name.parts.size() == 3)
                    throw wire::Error(Code::invalid_argument);
                name.parts.emplace_back(part);
                if (dot == remaining.npos) break;
                remaining.remove_prefix(dot + 1);
            }
            auto id = session_id();
            auto debugger = std::make_shared<sql::Debugger>(id, std::move(name), revision->get<uint32_t>());
            if (!debuggers_.emplace(id, DebugBinding{caller.id(), target->second, debugger}).second)
                throw wire::Error(Code::internal);
            target->second->attach_debugger(std::move(debugger));
            return wire::success(caller.request_number(), {{"debug_id", id}});
        }

        const auto& id = wire::string_field(body, "debug_id");
        const auto found = debuggers_.find(id);
        if (found == debuggers_.end() || found->second.controller != caller.id())
            throw wire::Error(Code::permission_denied);
        auto response = found->second.debugger->command(op, body);
        if (op == "debug_close")
        {
            if (auto target = found->second.target.lock()) target->attach_debugger({});
            debuggers_.erase(found);
        }
        return wire::success(caller.request_number(), std::move(response));
    }

    void close_debuggers(const std::shared_ptr<Session>& session)
    {
        for (auto item = debuggers_.begin(); item != debuggers_.end();)
        {
            const auto target = item->second.target.lock();
            if (item->second.controller == session->id() || !target || target == session)
            {
                item->second.debugger->cancel();
                if (target) target->attach_debugger({});
                item = debuggers_.erase(item);
            }
            else ++item;
        }
    }

    Json control(Session& caller, std::string_view op, const Json& body)
    {
        const auto& actor = *caller.principal();
        if (op.starts_with("debug_")) return debug_operation(caller, op, body);
        if (op == "admin_status")
        {
            if (!actor.administrator || !operations_) throw wire::Error(Code::permission_denied);
            if (!body.empty()) throw wire::Error(Code::invalid_argument);
            auto authenticated = Json::array();
            size_t transactions{}, waiting{};
            uint64_t slow{};
            for (const auto& [key, session] : sessions_)
            {
                if (!session->principal()) continue;
                auto info = session->summary();
                transactions += info.at("transaction_active").get<bool>() ? 1u : 0u;
                waiting += info.at("lock_wait_ms").get<uint64_t>() ? 1u : 0u;
                slow += session->slow_queries();
                authenticated.push_back(std::move(info));
            }
            const auto hdr = database_->current();
            return wire::success(caller.request_number(), {{"instance_name", config_.instance_name},
                {"instance_id", operations_->instance_id()}, {"port", acceptor_.local_endpoint().port()},
                {"sessions", std::move(authenticated)}, {"active_transactions", transactions}, {"waiting_sessions", waiting},
                {"slow_queries", slow}, {"disk", {{"pages", hdr.pages}, {"bytes", uint64_t{hdr.pages} * disk::page_size},
                    {"lsn", hdr.lsn}}}, {"limits", {{"connections", config_.max_connections}, {"timeout_ms", operations_->timeout().count()},
                    {"job_workers", 1}, {"sql_workers", 4}}}, {"deadlock_graph_available", false}});
        }
        auto found = sessions_.end();
        if (op == "session_list")
        {
            if (!security_->allowed(actor, "monitor")) throw wire::Error(Code::permission_denied);
            auto list = Json::array();
            for (const auto& entry : sessions_)
                if (entry.second->principal()) list.push_back(entry.second->summary());
            return wire::success(caller.request_number(), {{"sessions", std::move(list)}});
        }
        const auto& session_id = wire::string_field(body, "session_id");
        found = std::ranges::find_if(sessions_, [&](const auto& entry) { return entry.second->id() == session_id; });
        const bool privileged = security_->allowed(actor, "control");
        if (!privileged && (found == sessions_.end() || !found->second->principal() ||
            found->second->principal()->id != actor.id))
            throw wire::Error(Code::permission_denied);
        if (found == sessions_.end() || !found->second->principal() || found->second.get() == &caller)
            throw wire::Error(Code::invalid_state);
        if (op == "session_terminate")
        {
            found->second->stop();
            return wire::success(caller.request_number(), Json::object());
        }
        uint32_t request{};
        if (const auto value = body.find("request_id"); value != body.end())
        {
            if (!value->is_number_unsigned() || value->get<uint64_t>() > UINT32_MAX)
                throw wire::Error(Code::invalid_argument);
            request = value->get<uint32_t>();
        }
        return wire::success(caller.request_number(), {{"accepted", found->second->cancel_request(request)}});
    }

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
                const auto key = ++next_session_;
                auto session = std::make_shared<Session>(std::move(socket), tls_, config_, admin_, database_, key,
                    security_, jobs_, workers_, operations_.get(), [this](Session& caller, std::string_view op, const Json& body) {
                        return control(caller, op, body);
                    });
                sessions_.emplace(key, session);
                asio::co_spawn(io_, session->run(), [this, session, key](std::exception_ptr failure) {
                    if (failure) log_event(config_, false, "session_error");
                    close_debuggers(session);
                    sessions_.erase(key);
                });
            }
            accept();
        });
    }

    void watch_stop(std::stop_token token)
    {
        if (stopping_) return;
        if (token.stop_requested())
        {
            stop();
            return;
        }
        stop_poll_.expires_after(100ms);
        stop_poll_.async_wait([this, token](const asio::error_code& error) {
            if (!error) watch_stop(token);
        });
    }

    void stop()
    {
        if (stopping_) return;
        stopping_ = true;
        asio::error_code ignored;
        acceptor_.close(ignored);
        signals_.cancel(ignored);
        for (const auto& [key, session] : sessions_) session->stop();
    }

    const Config& config_;
    const Admin& admin_;
    disk::Engine* database_;
    sql::Security* security_;
    jobs::Manager* jobs_;
    std::unique_ptr<Operations> operations_;
    asio::io_context io_{1};
    asio::thread_pool workers_{4};
    asio::ssl::context tls_;
    tcp::acceptor acceptor_{io_};
    asio::steady_timer stop_poll_{io_};
    asio::signal_set signals_;
    std::map<uint64_t, std::shared_ptr<Session>> sessions_;
    std::map<std::string, DebugBinding, std::less<>> debuggers_;
    uint64_t next_session_{};
    bool stopping_{};
    bool failed_{};
};
}

int run(const Config& config, std::stop_token stop, const std::function<void()>& ready)
{
    asio::ssl::context tls(asio::ssl::context::tls_server);
    configure_tls(tls, config);
    const DataLock lock(config);
    try { check_storage_on_start(config); }
    catch (...)
    {
        log_diagnostic(config, "recovery", "recovery_failed");
        throw;
    }
    log_diagnostic(config, "recovery", "recovery_completed");
    const auto admin = load_admin(config);
    const auto path = config.data / store_name;
    std::unique_ptr<disk::Engine> database;
    if (std::filesystem::exists(path)) database = std::make_unique<disk::Engine>(path, disk::Hooks{}, config.decision_enabled);
    std::unique_ptr<sql::Security> security;
    if (database)
    {
        security = std::make_unique<sql::Security>(admin);
        const auto lsn = database->current().lsn;
        auto session = database->begin(UINT64_MAX, std::chrono::milliseconds{0});
        security->publish(security->snapshot(*session, UINT64_MAX), lsn);
    }
    const auto archives = database && !config.backup.empty() ?
        std::make_unique<backup::Archive>(*database, config.backup, config.temp) : nullptr;
    const auto scheduler = database ?
        std::make_unique<jobs::Manager>(*database, config.temp, config.mlp_cpu_enabled, archives.get()) : nullptr;
    Service service(config, admin, std::move(tls), database.get(), security.get(), scheduler.get());
    return service.run(stop, ready);
}
}
