/*
 * 文件作用：实现设备分片内的业务执行，是网络层与状态机/存储层的连接点。
 * run 在所属线程打开 SQLite、恢复本分片设备、等待输入并检查查询期限和数据年龄。
 * process 处理协议帧及断开通知，command 处理 status/query/task 等 CLI 请求。
 * 所有结果经过 WorkerOutput 返回 I/O；本文件不直接调用 send 或关闭业务 socket。
 * 阅读重点：110/410 的提交边界、911 的当前会话检查、正常排空和失败退出。
 */
#include "worker.hpp"

namespace batt {
Worker::Worker(std::size_t index, const WorkerConfig& config, BoundedQueue<WorkerOutput>& output,
               std::atomic<std::int64_t>& next_task, StoreShared& storage_shared)
    : index_(index), config_(config), input_(config.queue_capacity), output_(output),
      sessions_(1024 / config.workers, config.freshness), queries_(next_task, config.query_timeout),
      storage_shared_(storage_shared), thread_([this] { run(); }) {}
Worker::~Worker() { abort(); if (thread_.joinable()) thread_.join(); }
void Worker::abort() { aborted_.store(true); input_.close(); delay_wake_.notify_all(); }

// 公共返回队列满时业务线程可以等待；I/O 线程持续取走结果，不等待输入队列空位。
void Worker::emit(WorkerOutput output) {
    output.worker = index_;
    output.registered = sessions_.registered();
    for (auto& event : output.logs) event["worker"] = index_;
    output_.push_wait(std::move(output), aborted_);
}
void Worker::logs(std::vector<Json> events, Token token) {
    // Keep each output bounded even when listing many devices/alarms.
    for (auto& event : events) {
        WorkerOutput out; out.token = token; out.logs.push_back(std::move(event)); emit(std::move(out));
    }
}
void Worker::one(Json event, Token token) {
    WorkerOutput out; out.token = token; out.logs.push_back(std::move(event)); emit(std::move(out));
}

// SQLite 连接在实际使用它的线程中创建与释放；只恢复哈希归属本分片的设备。
void Worker::run() {
    bool started = false;
    try {
        store_ = std::make_unique<Store>(config_.storage, storage_shared_, aborted_);
        for (auto& device : store_->recover_devices()) {
            if (device_shard(device.name, config_.workers) == index_)
                sessions_.restore_device(device.name, std::move(device.open), device.total, device.next_alarm_id);
        }
        // 把查询编号分配和终态提交连接到 Store，确保生成 910 前 pending 已经持久化。
        queries_.persistence([this](const QueryTask& task) { return store_->create_query(task); },
                             [this](const QueryTask& task) { store_->finish_query(task); });
        ready_.set_value(); started = true;
        for (;;) {
            if (aborted_.load()) break;
            // 最多等待 10ms，然后处理一项并检查期限；这不是按最近 deadline 排序的调度器。
            auto work = input_.pop_for(std::chrono::milliseconds(10));
            if (work) { processing_.store(true); process(*work); processing_.store(false); }
            logs(queries_.expire(Clock::now()));
            pending_count_.store(queries_.stats().at("pending").get<std::size_t>());
            logs(sessions_.state_changes(Clock::now()));
            // 输入关闭且排空后，结束剩余 pending 并输出统计；已受理任务先完成再退出。
            if (input_.drained()) {
                logs(queries_.shutdown(Clock::now()));
                pending_count_.store(0);
                auto summary = queries_.stats(); summary["queue_peak"] = input_.peak();
                one(std::move(summary));
                break;
            }
        }
    } catch (const std::exception& error) {
        pending_count_.store(queries_.stats().at("pending").get<std::size_t>());
        storage_shared_.failed.store(true);
        if (!started) ready_.set_exception(std::current_exception());
        else try { logs({{{"event", "storage_error"}, {"reason", error.what()}}}); } catch (...) {}
        failed_.store(true);
    }
    store_.reset(); // SQLite connection is closed by its owning worker.
    done_.store(true);
}

// 断开、命令和数据帧在同一输入队列中串行处理，避免多个线程同时改同设备状态。
void Worker::process(const Work& work) {
    if (work.kind == WorkKind::disconnected) {
        logs(queries_.disconnect(work.token, work.reason, Clock::now()));
        sessions_.disconnect(work.token);
        return;
    }
    if (work.kind == WorkKind::command) { command(work); return; }
    const auto now = Clock::now();
    const auto type = work.frame.contains("msgType") ? integer(work.frame["msgType"]) : std::nullopt;
    // 旧连接排队中的重复登录不能重新夺回设备；首次登录还要受 accept 后 5 秒期限约束。
    if (type == std::optional<std::int64_t>(110) && !work.initial_login &&
        sessions_.device_for(work.token) != std::optional<std::string>(work.device)) {
        WorkerOutput out; out.token = work.token; out.frame_done = true;
        out.logs.push_back({{"event", "rejected"}, {"reason", "stale_login"}}); emit(std::move(out));
        return;
    }
    if (type == std::optional<std::int64_t>(110) && work.initial_login && now >= work.login_deadline) {
        WorkerOutput out; out.token = work.token; out.close_reason = "login_timeout"; out.frame_done = true;
        out.logs.push_back({{"event", "rejected"}, {"reason", "login_timeout"}}); emit(std::move(out));
        return;
    }
    // 显式测试钩子让本分片变慢，用于比较同/异分片延迟，不代表真实终端网络延迟。
    if (type == std::optional<std::int64_t>(310) && work.device == config_.slow_device && config_.slow_delay.count() > 0) {
        logs({{{"event", "test_delay_started"}, {"devId", work.device},
               {"txnNo", work.frame.value("txnNo", std::int64_t(0))}, {"delay_ms", config_.slow_delay.count()}}}, work.token);
        std::unique_lock<std::mutex> guard(delay_mutex_);
        delay_wake_.wait_for(guard, config_.slow_delay, [&] { return aborted_.load(); });
        if (aborted_.load()) return;
    }
    // 按 worker 此刻的单调时间判断响应；Sessions 再核对该 token 是否仍是当前连接。
    if (type == std::optional<std::int64_t>(911)) {
        const bool current = sessions_.device_for(work.token) == std::optional<std::string>(work.device);
        WorkerOutput out; out.token = work.token; out.frame_done = true;
        out.logs = queries_.response(work.token, work.frame, current, Clock::now()); emit(std::move(out));
        return;
    }
    std::vector<QueryTask> replaced;
    // 重连时把新登录和旧 pending 结束放进同一个事务；告警则提交前后集合的变化。
    auto commit = [&](const Sessions& candidate, const SessionReply& reply) {
        if (type == std::optional<std::int64_t>(110)) {
            if (reply.replaced) replaced = queries_.plan_disconnect(*reply.replaced, "replaced", Clock::now());
            store_->login(work.device, work.frame.at("imsi").get<std::string>(), replaced);
        } else {
            store_->alarms(work.device, sessions_.alarm_export(work.device), candidate.alarm_export(work.device));
        }
    };
    // 110/410 使用候选状态与数据库提交；310 只更新内存，因此三类 ACK 的持久化含义不同。
    auto result = (type == std::optional<std::int64_t>(110) || type == std::optional<std::int64_t>(410))
        ? sessions_.persisted_handle(work.token, work.frame, Clock::now(), commit)
        : sessions_.handle(work.token, work.frame, Clock::now());
    if (!replaced.empty()) logs(queries_.apply_committed(replaced));
    WorkerOutput out;
    out.token = work.token;
    out.frame_done = true;
    out.message = std::move(result.reply.ack);
    out.replaced = result.replaced;
    out.snapshot = sessions_.snapshot_for(work.token, Clock::now());
    out.logs = std::move(result.reply.events);
    if (type == std::optional<std::int64_t>(110) && out.message && (*out.message)["result"] == 1)
        out.bind = sessions_.device_for(work.token);
    if (result.reply.close_after_send) {
        out.close_reason = "protocol_close";
        logs(queries_.disconnect(work.token, "protocol_close", Clock::now()));
        sessions_.disconnect(work.token);
    }
    for (auto& event : out.logs) {
        event["queue_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(now - work.enqueued).count();
        if (event["event"] == "login" && out.snapshot) event["state"] = out.snapshot->at("state");
    }
    emit(std::move(out));
}

// CLI 也走设备分片，查询起点是这里调用 begin 的时刻；查看旧任务则直接读数据库。
void Worker::command(const Work& work) {
    if (work.api && work.api->expired()) return;
    Command action;
    if (work.action) action=*work.action;
    else {
        action.device=work.device; action.task_id=work.task_id;
        if (work.command=="query") action.kind=CommandKind::query;
        else if (work.command=="task") action.kind=CommandKind::task;
        else if (work.command=="alarms") action.kind=CommandKind::alarms;
        else if (work.command=="devices") action.kind=CommandKind::devices;
        else if (work.command=="stats") action.kind=CommandKind::stats;
    }
    auto reply = [&](ApiReply value) { if (work.api) work.api->complete(std::move(value)); };
    const auto now = Clock::now();
    if (action.kind == CommandKind::query) {
        QueryStart started;
        if (work.api) {
            const auto candidate = queries_.prepare(action.device,sessions_.snapshot_device(action.device,now),now);
            auto admission=store_->create_api_query(action.client_request_id,action.device,candidate);
            if (admission.replayed) {
                auto row=*admission.replayed; row["replayed"]=true;
                reply({200,std::move(row),""}); return;
            }
            started={admission.created,admission.rejected};
            if (started.task) queries_.publish(*started.task);
        } else started=queries_.begin(action.device,sessions_.snapshot_device(action.device,now),now);
        if (!started.task) {
            const int status=started.rejected=="not_found" ? 404 :
                (started.rejected=="busy" || started.rejected=="request_id_conflict" ? 409 : 503);
            reply(api_error(status,started.rejected,"Query not created"));
            if (!work.api) logs({{{"event", "query_rejected"}, {"devId", action.device}, {"reason", started.rejected}}});
            return;
        }
        const auto& task = *started.task;
        pending_count_.store(queries_.stats().at("pending").get<std::size_t>());
        WorkerOutput out;
        if (task.state == "pending" && finishing_.load()) {
            out.logs = queries_.disconnect(task.token, "server_shutdown", now);
        } else out.logs.push_back(Queries::describe(task, "query_state"));
        // pending 已提交后才生成 910；把同一个 deadline 带给发送缓存，排队不会延长期限。
        if (task.state == "pending" && !finishing_.load()) {
            out.token = task.token;
            out.deadline = task.deadline;
            out.message = Json{{"msgType", 910}, {"devId", task.device}, {"txnNo", task.id}};
        }
        emit(std::move(out));
        if (work.api) {
            auto row=*store_->task(task.id); row["replayed"]=false;
            reply({201,std::move(row),"/v1/queries/"+std::to_string(task.id)});
        }
    } else if (action.kind == CommandKind::task) {
        const auto task = store_->task(action.task_id);
        if (task) { reply({200,*task,""}); if (!work.api) one(*task); }
        else { reply(api_error(404,"task_not_found","Unknown query")); if (!work.api) logs({{{"event", "task_not_found"}, {"txnNo", action.task_id}}}); }
    } else if (action.kind == CommandKind::devices) {
        for (auto row : sessions_.snapshots(now)) { row["event"] = "device"; one(std::move(row)); }
        logs({{{"event", "devices_complete"}, {"count", sessions_.registered()}}});
    } else if (action.kind == CommandKind::status) {
        auto row = sessions_.snapshot_device(action.device, now);
        if (row) { reply({200,*row,""}); if (!work.api) { (*row)["event"] = "status"; one(std::move(*row)); } }
        else { reply(api_error(404,"device_not_found","Unknown device")); if (!work.api) logs({{{"event", "not_found"}, {"devId", action.device}}}); }
    } else if (action.kind == CommandKind::alarms) {
        if (work.api) {
            if (!sessions_.snapshot_device(action.device,now)) reply(api_error(404,"device_not_found","Unknown device"));
            else reply({200,store_->alarm_page(action.device,action.alarm_status,action.limit,action.before_id),""});
        } else {
            logs(store_->alarm_rows(action.device));
            logs({{{"event", "alarms_complete"}, {"devId", action.device}}});
        }
    } else if (action.kind == CommandKind::stats) {
        auto stats = queries_.stats(); stats["queue_peak"] = input_.peak(); one(std::move(stats));
    }
}

// 先同步完成建表与启动恢复，再启动 worker 并等待 ready；这些完成后服务才 bind/listen。
Workers::Workers(const WorkerConfig& config) {
    if (config.workers != 1 && config.workers != 2) throw std::invalid_argument("workers must be 1 or 2");
    std::atomic<bool> cancel{false};
    { Store bootstrap(config.storage, storage_shared_, cancel); bootstrap.initialize(); }
    try {
        for (std::size_t i = 0; i < config.workers; ++i)
            workers_.push_back(std::make_unique<Worker>(i, config, output_, next_task_, storage_shared_));
        for (auto& worker : workers_) worker->wait_ready();
    } catch (...) { abort(); workers_.clear(); throw; }
}
Workers::~Workers() { abort(); workers_.clear(); }
void Workers::finish() { for (auto& worker : workers_) worker->finish(); }
void Workers::abort() { output_.close(); for (auto& worker : workers_) worker->abort(); }
bool Workers::done() const {
    for (const auto& worker : workers_) if (!worker->done()) return false;
    return true;
}
bool Workers::failed() const {
    for (const auto& worker : workers_) if (worker->failed()) return true;
    return false;
}
Json Workers::queues() const {
    Json result = Json::array();
    for (std::size_t i = 0; i < workers_.size(); ++i)
        result.push_back({{"worker", i}, {"queued", workers_[i]->queued()}, {"peak", workers_[i]->peak()},
                          {"pending", workers_[i]->pending()}, {"processing", workers_[i]->processing()}});
    return result;
}
} // namespace batt
