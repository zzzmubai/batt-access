#pragma once
#include "bounded_queue.hpp"
#include "protocol.hpp"
#include <atomic>
#include <algorithm>
#include <memory>
#include <mutex>

namespace batt {
enum class CommandKind { status, query, task, alarms, devices, stats };
struct Command {
    CommandKind kind = CommandKind::status;
    std::string device, client_request_id, alarm_status;
    std::int64_t task_id = 0, before_id = std::numeric_limits<std::int64_t>::max();
    unsigned limit = 20;
};
struct ApiReply {
    int status = 200;
    Json body;
    std::string location;
};
inline ApiReply api_error(int status, const std::string& code, const std::string& message) {
    return {status, {{"error", {{"code", code}, {"message", message}}}}, ""};
}
// Shared value-only context: a timed-out HTTP callback never leaves a socket/reference in Work.
class ApiContext {
public:
    ApiContext(std::uint64_t request_id, Time until, bool is_post, std::string client_key)
        : id(request_id), deadline(until), post(is_post), key(std::move(client_key)) {}
    void complete(ApiReply value) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (result_) return;
        result_ = std::move(value); done_.store(true); wake_.notify_all();
    }
    ApiReply wait() {
        std::unique_lock<std::mutex> guard(mutex_);
        if (!wake_.wait_until(guard, deadline, [&] { return result_.has_value(); })) {
            auto reply = api_error(503, "request_deadline_exceeded", "Retry POST with the same clientRequestId");
            if (post) reply.body["outcome"] = "unknown";
            result_ = std::move(reply); done_.store(true);
        }
        auto reply = *result_;
        reply.body["requestId"] = id;
        if (post && !key.empty()) reply.body["clientRequestId"] = key;
        return reply;
    }
    bool expired() const { return done_.load() || Clock::now() >= deadline; }
    const std::uint64_t id;
    const Time deadline;
    const bool post;
    const std::string key;
private:
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<ApiReply> result_;
    std::atomic<bool> done_{false};
};
// A queued API request owns values and a shared completion context, never a socket.
struct ApiCall {
    Command command;
    std::shared_ptr<ApiContext> context;
};

class ApiBridge {
public:
    explicit ApiBridge(std::chrono::milliseconds wait = std::chrono::seconds(1), std::size_t capacity = 16)
        : input_(capacity), wait_(wait) {}
    ApiReply call(Command command) {
        const bool post = command.kind == CommandKind::query;
        auto context = std::make_shared<ApiContext>(next_.fetch_add(1), Clock::now()+wait_, post, command.client_request_id);
        {
            std::lock_guard<std::mutex> guard(mutex_);
            contexts_.erase(std::remove_if(contexts_.begin(), contexts_.end(),
                [](const auto& item) { return item.expired(); }), contexts_.end());
            if (closed_ || contexts_.size() >= 160 || !input_.try_push({std::move(command), context})) {
                ++rejected_;
                auto reply = api_error(503, closed_ ? "server_stopping" : "api_busy", "Command not admitted");
                if (post) reply.body["outcome"] = "not_created";
                context->complete(std::move(reply));
            } else { contexts_.push_back(context); ++admitted_; }
        }
        auto result = context->wait();
        if (result.status == 503 && result.body["error"]["code"] == "request_deadline_exceeded") ++timeouts_;
        return result;
    }
    std::optional<ApiCall> take() { return input_.try_pop(); }
    void close() {
        std::lock_guard<std::mutex> guard(mutex_);
        closed_ = true; input_.close();
        for (auto& weak : contexts_) if (auto context = weak.lock()) {
            auto reply = api_error(503, "server_stopping", "Retry POST with the same clientRequestId");
            if (context->post) reply.body["outcome"] = "unknown";
            context->complete(std::move(reply));
        }
    }
    Json stats() const { return {{"admitted",admitted_.load()},{"rejected",rejected_.load()},
        {"application_timeouts",timeouts_.load()},{"queue_peak",input_.peak()}}; }
private:
    BoundedQueue<ApiCall> input_;
    std::chrono::milliseconds wait_;
    std::atomic<std::uint64_t> next_{1}, admitted_{0}, rejected_{0}, timeouts_{0};
    std::mutex mutex_;
    std::vector<std::weak_ptr<ApiContext>> contexts_;
    bool closed_ = false;
};
} // namespace batt
