/* MySQL Store: candidate state -> transaction commit -> publish/ACK.
 * Connections/statements are owned by the calling Worker. No automatic reconnect
 * or write replay; an unconfirmed COMMIT is fatal and may already be durable.
 */
#include "store.hpp"
#include "mysql_raii.hpp"
#include "mysql_runtime.hpp"
#include "mysql_schema.hpp"
#include <unistd.h>
#include <thread>

namespace batt {
Store::Store(const StoreOptions& options, StoreShared& shared, const std::atomic<bool>& cancel)
    : db_(std::make_unique<MysqlConnection>(options.db)), options_(options), shared_(shared), cancel_(cancel) {}
Store::~Store() = default;
std::string Store::version() const { return mysql_client_version(); }
void Store::exec(const std::string& sql) { db_->exec(sql); }
std::int64_t Store::scalar(const std::string& sql) {
    MysqlStatement statement(*db_, sql);
    if (!statement.row()) throw StorageError("database_scalar_result_missing");
    return statement.number(0);
}
// 崩溃测试直接 _exit，不走析构；用于验证提交边界，正常配置不会触发。
void Store::checkpoint(const std::string& phase) {
    if (crash_this_ && options_.crash_phase == phase) ::_exit(86);
}
// 共享写锁从 BEGIN 持有到 COMMIT/ROLLBACK，不能带着此锁等待 socket 或返回队列。
void Store::write(const std::string& operation, const std::function<void()>& body) {
    lock_end_ = Clock::now() + std::chrono::milliseconds(500);
    std::unique_lock<std::mutex> lock(shared_.writer, std::defer_lock);
    try {
        if (shared_.failed.load()) throw StorageError("storage already failed");
        // Explicit bounded polling avoids the GCC 13 timed_mutex clocklock path,
        // whose acquisition is not intercepted by the bundled TSan runtime.
        while (!lock.try_lock()) {
            if (cancel_.load() || Clock::now() >= lock_end_) throw StorageError("database write lock timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (shared_.failed.load()) throw StorageError("storage already failed");
        if (cancel_.load() || Clock::now() >= lock_end_) throw StorageError("database operation cancelled");
        crash_this_ = options_.crash_operation == operation && shared_.crash_hits.fetch_add(1) == 0;
        // 先取得写事务再执行全部变更；故障测试可在提交前、提交后或告警处理中终止进程。
        MysqlTransaction transaction(*db_);
        body();
        checkpoint("before_commit");
        if (options_.fail_operation == operation && shared_.fail_hits.fetch_add(1) == 0)
            exec("INSERT INTO __injected_missing_table VALUES(1)");
        transaction.commit();
        checkpoint("after_commit");
    } catch (...) {
        shared_.failed.store(true); throw;
    }
    crash_this_ = false;
}

// 初始化脚本负责 DDL；运行账号仅核验结构并提交恢复 DML。
void Store::initialize() {
    validate_mysql_schema(*db_);
    write("recovery", [&] {
        MysqlStatement stmt(*db_, "UPDATE query_task SET status='link_lost',reason='server_restart',finished_ms=? WHERE status='pending'");
        stmt.number(1, wall_millis()); stmt.done();
    });
}
// 主键冲突只保留原 first_seen；更新最近登录与 imsi 和旧 pending 仍在一个事务内。
void Store::login(const std::string& dev, const std::string& imsi, const std::vector<QueryTask>& replaced) {
    write(replaced.empty() ? "login" : "login_replace", [&] {
        MysqlStatement insert(*db_, "INSERT INTO device(dev_id,imsi,first_seen_ms,last_login_ms) VALUES(?,?,?,?) ON DUPLICATE KEY UPDATE dev_id=device.dev_id");
        const auto now = wall_millis(); insert.text(1, dev); insert.text(2, imsi);
        insert.number(3, now); insert.number(4, now); insert.done();
        MysqlStatement update(*db_, "UPDATE device SET imsi=?,last_login_ms=? WHERE dev_id=?");
        update.text(1, imsi); update.number(2, now); update.text(3, dev); update.done();
        for (const auto& task : replaced) finish_in_transaction(task);
    });
}
// 按设备内部事件号对比候选与旧缓存，插入新事件、更新持续/恢复事件；整帧原子提交。
void Store::alarms(const std::string& dev, const std::vector<Alarm>& before, const std::vector<Alarm>& after) {
    std::map<std::uint64_t, const Alarm*> previous;
    for (const auto& row : before) previous[row.id] = &row;
    write("alarm", [&] {
        for (const auto& row : after) {
            if (!row.id || row.id > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                throw StorageError("alarm_event_number_exhausted");
            const auto old = previous.find(row.id);
            if (old != previous.end() && old->second->last_ms == row.last_ms && old->second->clear_ms == row.clear_ms) continue;
            if (old == previous.end()) {
                MysqlStatement stmt(*db_, "INSERT INTO alarm(dev_id,event_no,alarm_id,alarm_desc,status,first_ms,last_ms,clear_ms) VALUES(?,?,'01003001',?,?,?,?,?)");
                stmt.text(1, dev); stmt.number(2, static_cast<std::int64_t>(row.id)); stmt.text(3, row.desc);
                stmt.text(4, row.clear_ms ? "cleared" : "open"); stmt.number(5, row.first_ms); stmt.number(6, row.last_ms);
                if (row.clear_ms) stmt.number(7, *row.clear_ms); else stmt.null(7); stmt.done();
            } else {
                MysqlStatement stmt(*db_, "UPDATE alarm SET status=?,last_ms=?,clear_ms=? WHERE dev_id=? AND event_no=?");
                stmt.text(1, row.clear_ms ? "cleared" : "open"); stmt.number(2, row.last_ms);
                if (row.clear_ms) stmt.number(3, *row.clear_ms); else stmt.null(3);
                stmt.text(4, dev); stmt.number(5, static_cast<std::int64_t>(row.id)); stmt.done();
                if (stmt.affected_rows() != 1) throw StorageError("alarm row disappeared");
            }
            checkpoint("mid_alarm");
        }
    });
}
// 编号由 MySQL 分配，COMMIT 成功后才返回给 Queries，之后上层才能排队发送 910。
std::int64_t Store::create_query(const QueryTask& task) {
    std::int64_t id = 0;
    write("query_begin", [&] { id = insert_query(task, ""); });
    return id;
}
std::int64_t Store::insert_query(const QueryTask& task, const std::string& key) {
        MysqlStatement stmt(*db_, "INSERT INTO query_task(dev_id,connection_id,status,reason,created_ms,finished_ms,client_request_id) VALUES(?,?,?,?,?,?,?)");
        stmt.text(1, task.device);
        if (task.token) stmt.text(2, options_.boot_id + ":" + std::to_string(task.token)); else stmt.null(2);
        stmt.text(3, task.state); stmt.text(4, task.reason); stmt.number(5, task.created_ms);
        if (task.finished_ms) stmt.number(6, task.finished_ms); else stmt.null(6);
        if (key.empty()) stmt.null(7); else stmt.text(7, key);
        stmt.done();
        const auto id = stmt.insert_id();
        if (id <= 0) throw StorageError("query id exhausted");
        return id;
}
ApiAdmission Store::create_api_query(const std::string& key, const std::string& dev, const QueryStart& prepared) {
    ApiAdmission result;
    // Lookup and insert use the same writer transaction even across different device shards.
    // Expected conflicts are values, never exceptions through write()'s fatal-storage boundary.
    write("query_begin", [&] {
        MysqlStatement prior(*db_, "SELECT txn_no,dev_id FROM query_task WHERE client_request_id=?");
        prior.text(1, key);
        if (prior.row()) {
            if (prior.text(1) != dev) result.rejected = "request_id_conflict";
            else result.replayed = read_task(prior.number(0));
            return;
        }
        if (!prepared.task) { result.rejected = prepared.rejected; return; }
        auto task = *prepared.task;
        task.id = insert_query(task, key);
        result.created = std::move(task);
    });
    return result;
}
// WHERE status=pending 限制只能结束待决任务；影响行数不是 1 视为状态冲突。
void Store::finish_in_transaction(const QueryTask& task) {
    MysqlStatement stmt(*db_, "UPDATE query_task SET status=?,reason=?,finished_ms=?,response_json=? WHERE txn_no=? AND dev_id=? AND status='pending'");
    stmt.text(1, task.state); stmt.text(2, task.reason); stmt.number(3, task.finished_ms);
    if (task.response) stmt.text(4, task.response->dump()); else stmt.null(4);
    stmt.number(5, task.id); stmt.text(6, task.device); stmt.done();
    if (stmt.affected_rows() != 1) throw StorageError("query terminal state conflict");
}
void Store::finish_query(const QueryTask& task) { write("query_finish", [&] { finish_in_transaction(task); }); }

// 读取历史总数和最大事件号以延续生命周期，只将 open 行装回内存。
std::vector<RecoveredDevice> Store::recover_devices() {
    std::vector<RecoveredDevice> result;
    MysqlStatement devices(*db_, "SELECT dev_id FROM device ORDER BY dev_id");
    while (devices.row()) {
        RecoveredDevice row; row.name = devices.text(0);
        MysqlStatement counts(*db_, "SELECT COUNT(*),COALESCE(MAX(event_no),0) FROM alarm WHERE dev_id=?");
        counts.text(1, row.name); counts.row(); row.total = static_cast<std::uint64_t>(counts.number(0));
        row.next_alarm_id = static_cast<std::uint64_t>(counts.number(1)) + 1;
        MysqlStatement open(*db_, "SELECT event_no,alarm_desc,first_ms,last_ms FROM alarm WHERE dev_id=? AND status='open' ORDER BY event_no");
        open.text(1, row.name);
        while (open.row()) row.open.push_back({static_cast<std::uint64_t>(open.number(0)), open.text(1),
            Time{}, Time{}, std::nullopt, open.number(2), open.number(3), std::nullopt});
        result.push_back(std::move(row));
        if (result.size() > 1024) throw StorageError("device recovery capacity exceeded");
    }
    return result;
}
// CLI 从数据库查历史结果，即使旧终态已经被 Queries 的内存缓存逐出仍可查看。
std::optional<Json> Store::task(std::int64_t id) {
    return read_task(id);
}
std::optional<Json> Store::read_task(std::int64_t id) {
    MysqlStatement stmt(*db_, "SELECT txn_no,dev_id,connection_id,status,reason,created_ms,finished_ms,response_json,client_request_id FROM query_task WHERE txn_no=?");
    stmt.number(1, id); if (!stmt.row()) return std::nullopt;
    Json row = {{"event", "task"}, {"txnNo", stmt.number(0)}, {"devId", stmt.text(1)},
                {"connection_id", stmt.is_null(2) ? Json(nullptr) : Json(stmt.text(2))},
                {"state", stmt.text(3)}, {"reason", stmt.text(4)}, {"created_ms", stmt.number(5)},
                {"finished_ms", stmt.is_null(6) ? Json(nullptr) : Json(stmt.number(6))},
                {"response", stmt.is_null(7) ? Json(nullptr) : Json::parse(stmt.text(7))},
                {"clientRequestId", stmt.is_null(8) ? Json(nullptr) : Json(stmt.text(8))}};
    return row;
}
// 按设备读取最近 100 条告警供 CLI 展示；数据库中更早历史仍保留。
std::vector<Json> Store::alarm_rows(const std::string& dev) {
    MysqlStatement stmt(*db_, "SELECT id,event_no,alarm_desc,status,first_ms,last_ms,clear_ms FROM alarm WHERE dev_id=? ORDER BY id DESC LIMIT 100");
    stmt.text(1, dev); std::vector<Json> rows;
    while (stmt.row()) rows.push_back({{"event", "alarm"}, {"devId", dev}, {"id", stmt.number(0)},
        {"event_no", stmt.number(1)}, {"desc", stmt.text(2)}, {"state", stmt.text(3)},
        {"first_ms", stmt.number(4)}, {"last_ms", stmt.number(5)},
        {"clear_ms", stmt.is_null(6) ? Json(nullptr) : Json(stmt.number(6))}});
    return rows;
}
Json Store::alarm_page(const std::string& dev, const std::string& status, unsigned limit, std::int64_t before) {
    const auto sql = std::string("SELECT id,event_no,alarm_desc,status,first_ms,last_ms,clear_ms FROM alarm WHERE dev_id=? ") +
        (before > 0 ? "AND id<? " : "") +
        (status.empty() ? "" : "AND status=? ") + "ORDER BY id DESC LIMIT ?";
    MysqlStatement stmt(*db_, sql.c_str()); stmt.text(1, dev);
    int parameter = 2;
    if (before > 0) stmt.number(parameter++, before);
    if (!status.empty()) stmt.text(parameter++, status);
    stmt.number(parameter, static_cast<std::int64_t>(limit) + 1);
    auto rows = Json::array();
    while (stmt.row()) rows.push_back({{"id",stmt.number(0)},{"event_no",stmt.number(1)},{"devId",dev},
        {"desc",stmt.text(2)},{"state",stmt.text(3)},{"first_ms",stmt.number(4)},{"last_ms",stmt.number(5)},
        {"clear_ms",stmt.is_null(6) ? Json(nullptr) : Json(stmt.number(6))}});
    Json next = nullptr;
    if (rows.size() > limit) { rows.erase(rows.end()-1); next = rows.back()["id"]; }
    return {{"items",rows},{"nextBeforeId",next}};
}
} // namespace batt
