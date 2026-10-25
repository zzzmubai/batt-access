/*
 * 文件作用：实现服务端主动查询的任务状态机，由所属 worker 串行调用。
 * 状态流转：pending -> success / timeout / link_lost；终态不会被迟到或重复响应改写。
 * 同设备最多一个 pending；911 必须匹配设备、当前连接 token 和服务器分配的 txnNo。
 * 生产运行由 Store 回调分配持久化编号并提交结果；单测可直接使用内存编号。
 * 首版期限从 worker 创建任务时起算，包含提交和发送等待，不包含此前 CLI 入队等待。
 * 响应按 worker 实际处理时刻判断；及时进入队列也可能因处理延迟而超时。
 */
#pragma once
#include "session.hpp"
#include <atomic>

namespace batt {
// created/deadline 是进程内单调时间；created_ms/finished_ms 用于数据库持久化与展示。
struct QueryTask {
    std::int64_t id;
    std::string device;
    Token token;
    std::string state;
    std::string reason;
    Time created;
    Time deadline;
    std::optional<Json> response;
    bool late_seen = false;
    std::int64_t created_ms = 0, finished_ms = 0;
};
struct QueryStart {
    std::optional<QueryTask> task;
    std::string rejected;
};

class Queries {
public:
    Queries(std::atomic<std::int64_t>& next,
            std::chrono::milliseconds timeout = std::chrono::seconds(3),
            std::size_t limit = 1024) : next_(next), timeout_(timeout), limit_(limit) {}
    // 注入存储回调，使状态机本身不依赖 SQLite API；提交失败可通过异常阻止缓存更新。
    void persistence(std::function<std::int64_t(const QueryTask&)> allocate,
                     std::function<void(const QueryTask&)> commit) {
        allocate_ = std::move(allocate); commit_ = std::move(commit);
    }

    // 未知设备或已有 pending 时拒绝；已登记但不 fresh 时记录 link_lost 终态，不发送 910。
    QueryStart prepare(const std::string& dev, const std::optional<Json>& snapshot, Time now) {
        if (!snapshot) return {std::nullopt, "not_found"};
        if (pending_.count(dev)) return {std::nullopt, "busy"};
        // 启用持久化后可逐出旧终态缓存，历史仍可用 task 命令从数据库读取；pending 不逐出。
        if (tasks_.size() >= limit_ && allocate_) {
            for (auto it = tasks_.begin(); it != tasks_.end() && tasks_.size() >= limit_;)
                if (it->second.state != "pending") it = tasks_.erase(it); else ++it;
        }
        if (tasks_.size() >= limit_) return {std::nullopt, "memory_task_limit"};
        const auto token = snapshot->at("session_token").get<Token>();
        const auto availability = snapshot->at("state").get<std::string>();
        const bool fresh = availability == "online_fresh" && token != 0;
        QueryTask task{0, dev, token, fresh ? "pending" : "link_lost",
                       fresh ? "" : availability, now, now + timeout_, std::nullopt, false};
        task.created_ms = wall_millis(); task.finished_ms = fresh ? 0 : task.created_ms;
        return {task, ""};
    }
    void publish(const QueryTask& task) {
        tasks_.emplace(task.id, task);
        if (task.state == "pending") pending_[task.device] = task.id;
    }
    QueryStart begin(const std::string& dev, const std::optional<Json>& snapshot, Time now) {
        auto started = prepare(dev, snapshot, now);
        if (!started.task) return started;
        auto task = *started.task;
        const auto id = allocate_ ? allocate_(task) : next_.fetch_add(1);
        if (id <= 0 || id == std::numeric_limits<std::int64_t>::max())
            return {std::nullopt, "query_id_exhausted"};
        task.id = id;
        publish(task);
        return {task, ""};
    }
    const QueryTask* find(std::int64_t id) const {
        const auto found = tasks_.find(id);
        return found == tasks_.end() ? nullptr : &found->second;
    }
    // 当前时间达到或超过 deadline 即超时；具体终态先经 finish 提交，再移除 pending 索引。
    std::vector<Json> expire(Time now) {
        std::vector<Json> events;
        for (auto it = pending_.begin(); it != pending_.end();) {
            auto& task = tasks_.at(it->second);
            if (now >= task.deadline) {
                finish(task, "timeout", "deadline");
                events.push_back(describe(task, "query_state"));
                it = pending_.erase(it);
            } else ++it;
        }
        return events;
    }
    std::vector<Json> disconnect(Token token, const std::string& reason, Time now) {
        std::vector<Json> events;
        for (auto it = pending_.begin(); it != pending_.end();) {
            auto& task = tasks_.at(it->second);
            if (task.token != token) { ++it; continue; }
            finish(task, now >= task.deadline ? "timeout" : "link_lost",
                   now >= task.deadline ? "deadline" : reason);
            events.push_back(describe(task, "query_state"));
            it = pending_.erase(it);
        }
        return events;
    }
    // 重连替换先生成旧查询的结束计划，由 Store::login 与新登记放在同一个事务中提交。
    std::vector<QueryTask> plan_disconnect(Token token, const std::string& reason, Time now) const {
        std::vector<QueryTask> planned;
        for (const auto& item : pending_) {
            const auto& current = tasks_.at(item.second);
            if (current.token != token) continue;
            auto task = current;
            task.state = now >= task.deadline ? "timeout" : "link_lost";
            task.reason = now >= task.deadline ? "deadline" : reason;
            task.finished_ms = wall_millis(); planned.push_back(std::move(task));
        }
        return planned;
    }
    // 对应替换事务已经提交后才同步查询缓存，避免新登录失败却提前结束旧任务。
    std::vector<Json> apply_committed(const std::vector<QueryTask>& planned) {
        std::vector<Json> events;
        for (const auto& task : planned) {
            tasks_.at(task.id) = task; pending_.erase(task.device);
            events.push_back(describe(task, "query_state"));
        }
        return events;
    }
    std::vector<Json> shutdown(Time now) {
        std::vector<Token> tokens;
        for (const auto& item : pending_) tokens.push_back(tasks_.at(item.second).token);
        std::vector<Json> events;
        for (const auto token : tokens) {
            auto ended = disconnect(token, "server_shutdown", now);
            events.insert(events.end(), ended.begin(), ended.end());
        }
        return events;
    }

    // 先检查响应结构，再校验三重关联；不能仅凭 txnNo 就让任意连接完成查询。
    std::vector<Json> response(Token token, const Json& frame, bool current, Time now) {
        if (!frame.is_object() || !frame.contains("txnNo") || !frame.contains("devId") ||
            !identifier(frame["devId"]) || !integer(frame["txnNo"]) || *integer(frame["txnNo"]) <= 0 ||
            (frame.contains("attrList") && (!frame["attrList"].is_array() || frame["attrList"].size() > 256)))
            return {{{"event", "query_response_rejected"}, {"reason", "invalid_response"}}};
        if (frame.contains("attrList")) for (const auto& attr : frame["attrList"])
            if (!attr.is_object() || !attr.contains("id") || !identifier(attr["id"]))
                return {{{"event", "query_response_rejected"}, {"reason", "invalid_response"}}};
        const auto id = *integer(frame["txnNo"]);
        const auto found = tasks_.find(id);
        if (found == tasks_.end() || !current || found->second.token != token ||
            found->second.device != frame["devId"].get<std::string>()) {
            ++orphan_;
            return {{{"event", "orphan_response"}, {"txnNo", id}}};
        }
        auto& task = found->second;
        std::vector<Json> events;
        if (task.state == "pending") {
            if (now >= task.deadline) {
                finish(task, "timeout", "deadline");
                pending_.erase(task.device);
                events.push_back(describe(task, "query_state"));
            } else {
                finish(task, "success", "", frame);
                pending_.erase(task.device);
                events.push_back(describe(task, "query_state"));
                return events;
            }
        }
        // 超时后的帧只增加迟到统计；success 后重复帧、连接失效帧也都不能改变原终态。
        if (task.state == "timeout") {
            ++late_frames_;
            if (!task.late_seen) { task.late_seen = true; ++late_tasks_; }
            events.push_back({{"event", "late_response"}, {"txnNo", id}, {"devId", task.device}});
        } else if (task.state == "success") {
            ++duplicates_;
            events.push_back({{"event", "duplicate_response"}, {"txnNo", id}});
        } else {
            ++orphan_;
            events.push_back({{"event", "orphan_response"}, {"txnNo", id}});
        }
        return events;
    }

    static Json describe(const QueryTask& task, const std::string& event) {
        return {{"event", event}, {"txnNo", task.id}, {"devId", task.device},
                {"session_token", task.token}, {"state", task.state}, {"reason", task.reason},
                {"response", task.response ? *task.response : Json(nullptr)},
                {"created_ms", task.created_ms}, {"finished_ms", task.finished_ms ? Json(task.finished_ms) : Json(nullptr)}};
    }
    Json stats() const {
        return {{"event", "worker_stats"}, {"tasks", tasks_.size()}, {"pending", pending_.size()},
                {"late_frames", late_frames_}, {"late_tasks", late_tasks_},
                {"duplicate_response", duplicates_}, {"orphan_response", orphan_}};
    }
private:
    // 复制出下一状态 -> 调用提交回调 -> 替换缓存，保证数据库异常时不显示虚假的成功。
    void finish(QueryTask& current, const std::string& state, const std::string& reason,
                std::optional<Json> response = std::nullopt) {
        auto next = current; next.state = state; next.reason = reason;
        next.response = std::move(response); next.finished_ms = wall_millis();
        if (commit_) commit_(next);
        current = std::move(next);
    }
    std::atomic<std::int64_t>& next_;
    std::chrono::milliseconds timeout_;
    std::size_t limit_;
    std::map<std::int64_t, QueryTask> tasks_;
    std::map<std::string, std::int64_t> pending_;
    std::uint64_t late_frames_ = 0, late_tasks_ = 0, duplicates_ = 0, orphan_ = 0;
    std::function<std::int64_t(const QueryTask&)> allocate_;
    std::function<void(const QueryTask&)> commit_;
};
} // namespace batt
