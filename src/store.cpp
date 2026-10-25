/*
 * 文件作用：通过 SQLite C API 实现设备登记、告警和查询任务的持久化。
 * Statement 用 RAII 释放 prepared statement，并用参数绑定传入业务字段。
 * 数据库配置为 WAL、synchronous=FULL、foreign_keys=ON；写事务共享 500ms 等待预算。
 * 关键流程：write -> BEGIN IMMEDIATE -> 业务 SQL -> COMMIT；异常时回滚并标记存储失败。
 * initialize 负责旧 pending 恢复；recover_devices 只加载设备与 open 告警。
 * 数据库提交和网络应答无法原子完成，提交后 ACK 丢失时记录仍然保留。
 */
#include "store.hpp"
#include <climits>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>

namespace batt {
namespace {
// 每个语句对象负责 prepare/finalize；参数绑定避免把设备字段拼入 SQL。
// SQLITE_TRANSIENT 让 SQLite 复制字符串，调用者的临时字符串可以安全释放。
class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK)
            throw StorageError(std::string("prepare: ") + sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    void text(int n, const std::string& value) {
        if (sqlite3_bind_text(stmt_, n, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            throw StorageError("bind text failed");
    }
    void number(int n, std::int64_t value) {
        if (sqlite3_bind_int64(stmt_, n, value) != SQLITE_OK) throw StorageError("bind integer failed");
    }
    void null(int n) { if (sqlite3_bind_null(stmt_, n) != SQLITE_OK) throw StorageError("bind null failed"); }
    bool row() {
        const int rc = sqlite3_step(stmt_);
        if (rc != SQLITE_ROW && rc != SQLITE_DONE)
            throw StorageError(std::string("step: ") + sqlite3_errmsg(db_) + " (" + std::to_string(rc) + ")");
        return rc == SQLITE_ROW;
    }
    void done() { if (row()) throw StorageError("unexpected result row"); }
    std::int64_t number(int n) const { return sqlite3_column_int64(stmt_, n); }
    std::string text(int n) const {
        const auto* data = sqlite3_column_text(stmt_, n);
        return data ? std::string(reinterpret_cast<const char*>(data), static_cast<std::size_t>(sqlite3_column_bytes(stmt_, n))) : "";
    }
    bool is_null(int n) const { return sqlite3_column_type(stmt_, n) == SQLITE_NULL; }
private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};
// 三张业务表加约束：设备外键、同设备同描述最多一条 open、同设备最多一个 pending。
// alarm.event_no 是设备内部事件号；query_task.txn_no 是数据库分配的全局查询编号。
const char* schema = R"SQL(
CREATE TABLE device(dev_id TEXT PRIMARY KEY, imsi TEXT NOT NULL,
 first_seen_ms INTEGER NOT NULL, last_login_ms INTEGER NOT NULL);
CREATE TABLE alarm(id INTEGER PRIMARY KEY, dev_id TEXT NOT NULL REFERENCES device(dev_id),
 event_no INTEGER NOT NULL CHECK(event_no>0), alarm_id TEXT NOT NULL, alarm_desc TEXT NOT NULL,
 status TEXT NOT NULL CHECK(status IN ('open','cleared')), first_ms INTEGER NOT NULL,
 last_ms INTEGER NOT NULL, clear_ms INTEGER, UNIQUE(dev_id,event_no),
 CHECK((status='open' AND clear_ms IS NULL) OR (status='cleared' AND clear_ms IS NOT NULL)));
CREATE UNIQUE INDEX one_open_alarm ON alarm(dev_id,alarm_id,alarm_desc) WHERE status='open';
CREATE TABLE query_task(txn_no INTEGER PRIMARY KEY AUTOINCREMENT,
 dev_id TEXT NOT NULL REFERENCES device(dev_id), connection_id TEXT,
 status TEXT NOT NULL CHECK(status IN ('pending','success','timeout','link_lost')),
 reason TEXT, created_ms INTEGER NOT NULL, finished_ms INTEGER, response_json TEXT,
 CHECK((status='pending' AND finished_ms IS NULL) OR (status<>'pending' AND finished_ms IS NOT NULL)));
CREATE UNIQUE INDEX one_pending_query ON query_task(dev_id) WHERE status='pending';
PRAGMA application_id=1111577684;
PRAGMA user_version=1;
)SQL";
}

// 规范化数据库路径，配合 access.cpp 的 .lock 文件限制同一库由本服务一个进程持有。
std::string canonical_database(const std::string& path) {
    if (path.empty() || path == ":memory:") throw StorageError("a local database file is required");
    char resolved[PATH_MAX];
    if (::realpath(path.c_str(), resolved)) {
        struct stat info{};
        if (::stat(resolved, &info) != 0 || !S_ISREG(info.st_mode)) throw StorageError("database must be a regular file");
        return resolved;
    }
    const auto slash = path.find_last_of('/');
    const auto parent = slash == std::string::npos ? "." : (slash == 0 ? "/" : path.substr(0, slash));
    const auto name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (name.empty() || name == "." || name == ".." || !::realpath(parent.c_str(), resolved))
        throw StorageError("database parent directory is unavailable");
    return std::string(resolved) + "/" + name;
}

// 每个连接都启用并验证数据库配置；FULLMUTEX 保护 SQLite API，业务事务仍需共享写锁。
Store::Store(const StoreOptions& options, StoreShared& shared, const std::atomic<bool>& cancel)
    : options_(options), shared_(shared), cancel_(cancel) {
    const int rc = sqlite3_open_v2(options.path.c_str(), &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        const std::string reason = db_ ? sqlite3_errmsg(db_) : "allocation failed";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw StorageError("open: " + reason);
    }
    try {
        sqlite3_extended_result_codes(db_, 1);
        sqlite3_busy_handler(db_, &Store::busy, this);
        sqlite3_progress_handler(db_, 1000, &Store::progress, this);
        operation_end_ = Clock::now() + std::chrono::milliseconds(500);
        Statement wal(db_, "PRAGMA journal_mode=WAL");
        if (!wal.row() || wal.text(0) != "wal") throw StorageError("WAL was not enabled");
        while (wal.row()) {}
        exec("PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;");
        if (scalar("PRAGMA synchronous") != 2 || scalar("PRAGMA foreign_keys") != 1)
            throw StorageError("required database settings were not applied");
    } catch (...) { sqlite3_close_v2(db_); db_ = nullptr; throw; }
}
Store::~Store() { if (db_) sqlite3_close_v2(db_); }
// SQLite 遇锁时短暂等待，但不能超过整个操作的期限；停止标志也能终止等待。
int Store::busy(void* pointer, int) {
    auto& store = *static_cast<Store*>(pointer);
    if (store.cancel_.load() || Clock::now() >= store.operation_end_) return 0;
    sqlite3_sleep(5); return 1;
}
// 长 SQL 执行期间检查取消/期限；这不能保证中断操作系统中不可中断的磁盘 I/O。
int Store::progress(void* pointer) {
    auto& store = *static_cast<Store*>(pointer);
    return store.cancel_.load() || Clock::now() >= store.operation_end_ ? 1 : 0;
}
void Store::exec(const std::string& sql) {
    char* message = nullptr;
    const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        const std::string reason = message ? message : sqlite3_errmsg(db_); sqlite3_free(message);
        throw StorageError(reason + " (" + std::to_string(rc) + ")");
    }
}
std::int64_t Store::scalar(const std::string& sql) {
    Statement stmt(db_, sql.c_str());
    if (!stmt.row()) throw StorageError("scalar result missing");
    return stmt.number(0);
}
// 崩溃测试直接 _exit，不走析构；用于验证提交边界，正常配置不会触发。
void Store::checkpoint(const std::string& phase) {
    if (crash_this_ && options_.crash_phase == phase) ::_exit(86);
}
// 共享写锁从 BEGIN 持有到 COMMIT/ROLLBACK，不能带着此锁等待 socket 或返回队列。
void Store::write(const std::string& operation, const std::function<void()>& body) {
    operation_end_ = Clock::now() + std::chrono::milliseconds(500);
    std::unique_lock<std::timed_mutex> lock(shared_.writer, std::defer_lock);
    try {
        if (shared_.failed.load()) throw StorageError("storage already failed");
        while (!lock.try_lock_for(std::chrono::milliseconds(5)))
            if (cancel_.load() || Clock::now() >= operation_end_) throw StorageError("database write lock timeout");
        if (shared_.failed.load()) throw StorageError("storage already failed");
        if (cancel_.load() || Clock::now() >= operation_end_) throw StorageError("database operation cancelled");
        crash_this_ = options_.crash_operation == operation && shared_.crash_hits.fetch_add(1) == 0;
        // 先取得写事务再执行全部变更；故障测试可在提交前、提交后或告警处理中终止进程。
        exec("BEGIN IMMEDIATE");
        body();
        checkpoint("before_commit");
        if (options_.fail_operation == operation && shared_.fail_hits.fetch_add(1) == 0)
            exec("INSERT INTO __injected_missing_table VALUES(1)");
        exec("COMMIT");
        checkpoint("after_commit");
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        shared_.failed.store(true); throw;
    }
    crash_this_ = false;
}

// 只初始化本项目的库；不把未知已有数据库当成空库覆盖，也不自动重发重启前的查询。
void Store::initialize() {
    write("recovery", [&] {
        auto version = scalar("PRAGMA user_version");
        if (version == 0) {
            if (scalar("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'") != 0)
                throw StorageError("refusing an unrelated unversioned database");
            exec(schema);
            version = 1;
        } else if ((version != 1 && version != 2) || scalar("PRAGMA application_id") != 1111577684)
            throw StorageError("unsupported database schema");
        if (version == 1) {
            exec("ALTER TABLE query_task ADD COLUMN client_request_id TEXT");
            checkpoint("mid_migration");
            exec("CREATE UNIQUE INDEX unique_client_request ON query_task(client_request_id) WHERE client_request_id IS NOT NULL;"
                 "CREATE INDEX alarm_device_id ON alarm(dev_id,id DESC);"
                 "CREATE INDEX alarm_device_status_id ON alarm(dev_id,status,id DESC); PRAGMA user_version=2;");
        }
        Statement stmt(db_, "UPDATE query_task SET status='link_lost',reason='server_restart',finished_ms=? WHERE status='pending'");
        stmt.number(1, wall_millis()); stmt.done();
    });
}
// INSERT OR IGNORE 保留 first_seen，再更新最近登录与 imsi；旧任务结束也在此事务内。
void Store::login(const std::string& dev, const std::string& imsi, const std::vector<QueryTask>& replaced) {
    write(replaced.empty() ? "login" : "login_replace", [&] {
        Statement insert(db_, "INSERT OR IGNORE INTO device VALUES(?,?,?,?)");
        const auto now = wall_millis(); insert.text(1, dev); insert.text(2, imsi);
        insert.number(3, now); insert.number(4, now); insert.done();
        Statement update(db_, "UPDATE device SET imsi=?,last_login_ms=? WHERE dev_id=?");
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
            const auto old = previous.find(row.id);
            if (old != previous.end() && old->second->last_ms == row.last_ms && old->second->clear_ms == row.clear_ms) continue;
            if (old == previous.end()) {
                Statement stmt(db_, "INSERT INTO alarm(dev_id,event_no,alarm_id,alarm_desc,status,first_ms,last_ms,clear_ms) VALUES(?,?,'01003001',?,?,?,?,?)");
                stmt.text(1, dev); stmt.number(2, static_cast<std::int64_t>(row.id)); stmt.text(3, row.desc);
                stmt.text(4, row.clear_ms ? "cleared" : "open"); stmt.number(5, row.first_ms); stmt.number(6, row.last_ms);
                if (row.clear_ms) stmt.number(7, *row.clear_ms); else stmt.null(7); stmt.done();
            } else {
                Statement stmt(db_, "UPDATE alarm SET status=?,last_ms=?,clear_ms=? WHERE dev_id=? AND event_no=?");
                stmt.text(1, row.clear_ms ? "cleared" : "open"); stmt.number(2, row.last_ms);
                if (row.clear_ms) stmt.number(3, *row.clear_ms); else stmt.null(3);
                stmt.text(4, dev); stmt.number(5, static_cast<std::int64_t>(row.id)); stmt.done();
                if (sqlite3_changes(db_) != 1) throw StorageError("alarm row disappeared");
            }
            checkpoint("mid_alarm");
        }
    });
}
// 编号由 SQLite 分配，COMMIT 成功后才返回给 Queries，之后上层才能排队发送 910。
std::int64_t Store::create_query(const QueryTask& task) {
    std::int64_t id = 0;
    write("query_begin", [&] { id = insert_query(task, ""); });
    return id;
}
std::int64_t Store::insert_query(const QueryTask& task, const std::string& key) {
        Statement stmt(db_, "INSERT INTO query_task(dev_id,connection_id,status,reason,created_ms,finished_ms,client_request_id) VALUES(?,?,?,?,?,?,?)");
        stmt.text(1, task.device);
        if (task.token) stmt.text(2, options_.boot_id + ":" + std::to_string(task.token)); else stmt.null(2);
        stmt.text(3, task.state); stmt.text(4, task.reason); stmt.number(5, task.created_ms);
        if (task.finished_ms) stmt.number(6, task.finished_ms); else stmt.null(6);
        if (key.empty()) stmt.null(7); else stmt.text(7, key);
        stmt.done();
        const auto id = sqlite3_last_insert_rowid(db_);
        if (id <= 0 || id == std::numeric_limits<std::int64_t>::max()) throw StorageError("query id exhausted");
        return id;
}
ApiAdmission Store::create_api_query(const std::string& key, const std::string& dev, const QueryStart& prepared) {
    ApiAdmission result;
    // Lookup and insert use the same writer transaction even across different device shards.
    // Expected conflicts are values, never exceptions through write()'s fatal-storage boundary.
    write("query_begin", [&] {
        Statement prior(db_, "SELECT txn_no,dev_id FROM query_task WHERE client_request_id=?");
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
    Statement stmt(db_, "UPDATE query_task SET status=?,reason=?,finished_ms=?,response_json=? WHERE txn_no=? AND dev_id=? AND status='pending'");
    stmt.text(1, task.state); stmt.text(2, task.reason); stmt.number(3, task.finished_ms);
    if (task.response) stmt.text(4, task.response->dump()); else stmt.null(4);
    stmt.number(5, task.id); stmt.text(6, task.device); stmt.done();
    if (sqlite3_changes(db_) != 1) throw StorageError("query terminal state conflict");
}
void Store::finish_query(const QueryTask& task) { write("query_finish", [&] { finish_in_transaction(task); }); }

// 读取历史总数和最大事件号以延续生命周期，只将 open 行装回内存。
std::vector<RecoveredDevice> Store::recover_devices() {
    operation_end_ = Clock::now() + std::chrono::milliseconds(500);
    std::vector<RecoveredDevice> result;
    Statement devices(db_, "SELECT dev_id FROM device ORDER BY dev_id");
    while (devices.row()) {
        RecoveredDevice row; row.name = devices.text(0);
        Statement counts(db_, "SELECT COUNT(*),COALESCE(MAX(event_no),0) FROM alarm WHERE dev_id=?");
        counts.text(1, row.name); counts.row(); row.total = static_cast<std::uint64_t>(counts.number(0));
        row.next_alarm_id = static_cast<std::uint64_t>(counts.number(1)) + 1;
        Statement open(db_, "SELECT event_no,alarm_desc,first_ms,last_ms FROM alarm WHERE dev_id=? AND status='open' ORDER BY event_no");
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
    operation_end_ = Clock::now() + std::chrono::milliseconds(500);
    return read_task(id);
}
std::optional<Json> Store::read_task(std::int64_t id) {
    Statement stmt(db_, "SELECT txn_no,dev_id,connection_id,status,reason,created_ms,finished_ms,response_json,client_request_id FROM query_task WHERE txn_no=?");
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
    operation_end_ = Clock::now() + std::chrono::milliseconds(500);
    Statement stmt(db_, "SELECT id,event_no,alarm_desc,status,first_ms,last_ms,clear_ms FROM alarm WHERE dev_id=? ORDER BY id DESC LIMIT 100");
    stmt.text(1, dev); std::vector<Json> rows;
    while (stmt.row()) rows.push_back({{"event", "alarm"}, {"devId", dev}, {"id", stmt.number(0)},
        {"event_no", stmt.number(1)}, {"desc", stmt.text(2)}, {"state", stmt.text(3)},
        {"first_ms", stmt.number(4)}, {"last_ms", stmt.number(5)},
        {"clear_ms", stmt.is_null(6) ? Json(nullptr) : Json(stmt.number(6))}});
    return rows;
}
Json Store::alarm_page(const std::string& dev, const std::string& status, unsigned limit, std::int64_t before) {
    operation_end_ = Clock::now() + std::chrono::milliseconds(500);
    const auto sql = std::string("SELECT id,event_no,alarm_desc,status,first_ms,last_ms,clear_ms FROM alarm WHERE dev_id=? AND id<? ") +
        (status.empty() ? "" : "AND status=? ") + "ORDER BY id DESC LIMIT ?";
    Statement stmt(db_, sql.c_str()); stmt.text(1, dev); stmt.number(2, before);
    int parameter = 3;
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
