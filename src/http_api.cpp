// HTTP/1.1 is provided by the pinned library; this file only adapts typed commands.
#define CPPHTTPLIB_HEADER_MAX_LENGTH 1024
#define CPPHTTPLIB_HEADER_MAX_COUNT 16
#define CPPHTTPLIB_REQUEST_URI_MAX_LENGTH 2048
#define CPPHTTPLIB_MAX_LINE_LENGTH 2048
#include <httplib.h>
#include "http_api.hpp"
#include "net.hpp"
#include <array>
#include <cctype>
#include <fcntl.h>
#include <set>
#include <thread>

namespace batt {
namespace {
// A small protected-access vendor patch permits this lifecycle hook. Hold duplicated
// descriptors while serving: stop must never shutdown a reused device fd number.
class TrackedServer : public httplib::Server {
public:
    void stop_connections() {
        std::lock_guard<std::mutex> guard(mutex_); stopping_ = true;
        stop();
        for (const auto fd : active_) ::shutdown(fd, SHUT_RDWR);
    }
protected:
    bool process_and_close_socket(int sock) override {
        Fd duplicate(::fcntl(sock, F_DUPFD_CLOEXEC, 0));
        if (!duplicate) ::shutdown(sock, SHUT_RDWR); // no untracked blocking read on descriptor exhaustion
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (duplicate) { active_.insert(duplicate.get()); if (stopping_) ::shutdown(duplicate.get(), SHUT_RDWR); }
        }
        const bool result = httplib::Server::process_and_close_socket(sock);
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (duplicate) active_.erase(duplicate.get());
        }
        return result;
    }
private:
    std::mutex mutex_;
    std::set<int> active_;
    bool stopping_ = false;
};
bool positive(const std::string& value, std::int64_t& out) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) return false;
    try { out = std::stoll(value); return out > 0; } catch (...) { return false; }
}
bool valid_key(const std::string& key) {
    return !key.empty() && key.size() <= 64 &&
        key.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") == std::string::npos;
}
}
struct HttpGateway::Impl {
    ApiBridge& bridge;
    HttpOptions options;
    TrackedServer server;
    std::thread listener;
    unsigned actual_port = 0;
    std::atomic<bool> stopping{false};
    std::atomic<bool> listener_done{false};
    std::atomic<std::uint64_t> count{0}, created{0}, replayed{0}, busy{0}, errors{0};
    std::mutex delay_mutex;
    std::condition_variable delay_wake;

    Impl(ApiBridge& input, const HttpOptions& config) : bridge(input), options(config) {
        server.new_task_queue = [] { return new httplib::ThreadPool(2, 0, 8); };
        // Upstream enables SO_REUSEPORT on Linux by default; this service must
        // reject a second listener instead of silently load-balancing two databases.
        server.set_socket_options([](int socket) {
            const int yes=1;
            if (::setsockopt(socket,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes))<0) throw sys_error("HTTP reuseaddr");
        });
        server.set_read_timeout(2); server.set_write_timeout(2);
        server.set_payload_max_length(4096); server.set_keep_alive_max_count(1);
        server.set_error_handler([](const httplib::Request&, httplib::Response& response) {
            if (response.body.empty()) response.set_content(api_error(response.status, "http_error", "HTTP parsing or routing error").body.dump(), "application/json");
        });
        auto handler = [this](const httplib::Request& request, httplib::Response& response) { handle(request,response); };
        server.Get(R"(/.*)",handler); server.Post(R"(/.*)",handler);
        server.Put(R"(/.*)",handler); server.Delete(R"(/.*)",handler);
        server.Patch(R"(/.*)",handler); server.Options(R"(/.*)",handler);
        const int bound = options.port == 0 ? server.bind_to_any_port("127.0.0.1") :
            (server.bind_to_port("127.0.0.1", options.port) ? static_cast<int>(options.port) : -1);
        if (bound <= 0) throw std::runtime_error("HTTP bind failed");
        actual_port = static_cast<unsigned>(bound);
        listener = std::thread([this] { server.listen_after_bind(); listener_done.store(true); });
        const auto deadline=Clock::now()+std::chrono::seconds(2);
        while (!server.is_running() && !listener_done.load() && Clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!server.is_running()) {
            stop_all(); if (listener.joinable()) listener.join();
            throw std::runtime_error("HTTP listener failed");
        }
    }
    ~Impl() { stop_all(); if (listener.joinable()) listener.join(); }
    void stop_all() {
        if (!stopping.exchange(true)) { bridge.close(); server.stop_connections(); delay_wake.notify_all(); }
    }
    void respond(const httplib::Request& request, httplib::Response& response, ApiReply reply) {
        if (!reply.body.is_object()) reply = api_error(500,"invalid_result","Expected object result");
        auto body = reply.body.dump();
        if (body.size() > 256*1024) { reply = api_error(500,"response_too_large","Result exceeds bound"); body = reply.body.dump(); }
        response.status = reply.status;
        response.set_header("Cache-Control","no-store");
        response.set_header("Connection","close");
        if (!reply.location.empty()) response.set_header("Location", reply.location);
        if (reply.status == 405) response.set_header("Allow", request.path == "/v1/queries" ? "POST" : "GET");
        if (reply.status == 503) response.set_header("Retry-After","1");
        response.set_content(body, "application/json");
        ++count;
        if (reply.status == 201) ++created;
        else if (reply.status == 200 && reply.body.value("replayed",false)) ++replayed;
        else if (reply.status == 409) ++busy;
        else if (reply.status >= 400) ++errors;
    }
    void handle(const httplib::Request& request, httplib::Response& response) {
        Command command;
        ApiReply error = api_error(400,"invalid_request","Invalid parameters");
        bool valid = true, known = true;
        const auto& path = request.path;
        if (path == "/v1/queries") command.kind = CommandKind::query;
        else if (path.compare(0,12,"/v1/devices/") == 0) { command.kind=CommandKind::status; command.device=path.substr(12); }
        else if (path.compare(0,12,"/v1/queries/") == 0) { command.kind=CommandKind::task; valid=positive(path.substr(12),command.task_id); }
        else if (path == "/v1/alarms") command.kind=CommandKind::alarms;
        else known = false;
        if (!known) { respond(request,response,api_error(404,"route_not_found","Unknown route")); return; }
        if (request.method != (command.kind == CommandKind::query ? "POST" : "GET")) {
            respond(request,response,api_error(405,"method_not_allowed","Unsupported method")); return;
        }
        if (command.kind == CommandKind::query) {
            auto media = request.get_header_value("Content-Type");
            const auto semi = media.find(';'); if (semi != std::string::npos) media.resize(semi);
            const auto first=media.find_first_not_of(" \t");
            media=first==std::string::npos ? "" : media.substr(first,media.find_last_not_of(" \t")-first+1);
            std::transform(media.begin(),media.end(),media.begin(),[](unsigned char ch){return static_cast<char>(std::tolower(ch));});
            if (media != "application/json") { respond(request,response,api_error(415,"unsupported_media_type","Use application/json")); return; }
            std::set<std::string> keys;
            bool duplicate = false;
            auto body = Json::parse(request.body, [&](int depth, Json::parse_event_t event, Json& value) {
                if (event == Json::parse_event_t::key && depth == 1 && !keys.insert(value.get<std::string>()).second) duplicate=true;
                return true;
            }, false);
            if (!duplicate && body.is_object() && body.contains("clientRequestId") && body["clientRequestId"].is_string()) {
                const auto key=body["clientRequestId"].get<std::string>();
                if (valid_key(key)) command.client_request_id=key;
            }
            valid = valid && !duplicate && body.is_object() && body.size()==2 && body.contains("devId") && body.contains("clientRequestId") &&
                body["clientRequestId"].is_string() && identifier(body["devId"]);
            if (valid) {
                command.device=body["devId"].get<std::string>();
                valid=!command.client_request_id.empty();
            }
        }
        if (command.kind == CommandKind::alarms) {
            const std::set<std::string> allowed={"devId","status","limit","beforeId"};
            for (const auto& parameter : request.params) if (!allowed.count(parameter.first) || request.get_param_value_count(parameter.first)!=1) valid=false;
            command.device=request.get_param_value("devId"); command.alarm_status=request.get_param_value("status");
            if (request.has_param("status") && command.alarm_status!="open" && command.alarm_status!="cleared") valid=false;
            std::int64_t size=20;
            if (request.has_param("limit") && (!positive(request.get_param_value("limit"),size) || size>50)) valid=false;
            if (size>0 && size<=50) command.limit=static_cast<unsigned>(size);
            if (request.has_param("beforeId") && !positive(request.get_param_value("beforeId"),command.before_id)) valid=false;
        } else if (!request.params.empty()) valid=false;
        if (command.kind == CommandKind::status || command.kind == CommandKind::alarms) valid=valid && identifier(Json(command.device));
        if (!valid) {
            if (!command.client_request_id.empty()) error.body["clientRequestId"]=command.client_request_id;
            respond(request,response,std::move(error)); return;
        }
        auto reply = bridge.call(std::move(command));
        if (options.reply_delay.count()>0 && request.method=="POST" && reply.status==201) {
            std::unique_lock<std::mutex> lock(delay_mutex);
            delay_wake.wait_for(lock,options.reply_delay,[this]{return stopping.load();});
        }
        respond(request,response,std::move(reply));
    }
};
HttpGateway::HttpGateway(ApiBridge& bridge, const HttpOptions& options) : impl_(std::make_unique<Impl>(bridge,options)) {}
HttpGateway::~HttpGateway() = default;
void HttpGateway::stop() { impl_->stop_all(); }
unsigned HttpGateway::port() const { return impl_->actual_port; }
Json HttpGateway::stats() const { return {{"http_requests",impl_->count.load()},{"created",impl_->created.load()},
    {"replayed",impl_->replayed.load()},{"conflicts",impl_->busy.load()},{"errors",impl_->errors.load()}}; }
}
