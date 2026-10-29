/*
 * 文件作用：声明 MySQL 持久化边界，供 worker.cpp 使用，实现见 store.cpp。
 * 保存 device 设备档案、alarm 告警生命周期、query_task 查询记录；属性快照只在内存中。
 * 启动连接核验已初始化结构并结束旧 pending，之后每个 worker 持有自己的连接。
 * StoreShared 的写锁串行化本进程关键写事务。
 * 存储错误通过 StorageError 向上传播，服务停止受理并失败退出。
 */
#pragma once
#include "query_task.hpp"
#include "storage_error.hpp"
#include "db_config.hpp"
#include <memory>
#include <mutex>

namespace batt {
// boot_id 与连接 token 组合成可跨进程区分的 connection_id；fail/crash 字段仅用于故障测试。
struct StoreOptions {
    DbConfig db;
    std::string config_path, boot_id;
    std::string fail_operation, crash_operation, crash_phase;
};
// 所有 Store 共享有限时写锁和失败标记；任一写入失败后，其他写入也不再继续受理。
struct StoreShared {
    std::mutex writer;
    std::atomic<bool> failed{false};
    std::atomic<unsigned> fail_hits{0}, crash_hits{0};
};
// 恢复包只携带设备名、open 告警及事件计数，连接和旧属性的新鲜度不恢复。
struct RecoveredDevice {
    std::string name;
    std::vector<Alarm> open;
    std::uint64_t total = 0, next_alarm_id = 1;
};
struct ApiAdmission {
    std::optional<QueryTask> created;
    std::optional<Json> replayed;
    std::string rejected;
};

class MysqlConnection;
class Store {
public:
    Store(const StoreOptions& options, StoreShared& shared, const std::atomic<bool>& cancel);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    // 服务启动前执行：核验外部初始化的结构，把旧 pending 结束为 link_lost/server_restart。
    void initialize();
    // 合法接管时，新登记和旧查询结束由一次事务提交。
    void login(const std::string& dev, const std::string& imsi, const std::vector<QueryTask>& replaced);
    void alarms(const std::string& dev, const std::vector<Alarm>& before, const std::vector<Alarm>& after);
    // 提交后返回 AUTOINCREMENT 编号；保留同一数据库才能延续跨重启编号。
    std::int64_t create_query(const QueryTask& task);
    ApiAdmission create_api_query(const std::string& key, const std::string& dev, const QueryStart& prepared);
    Json alarm_page(const std::string& dev, const std::string& status, unsigned limit, std::int64_t before);
    void finish_query(const QueryTask& task);
    std::vector<RecoveredDevice> recover_devices();
    std::optional<Json> task(std::int64_t id);
    std::vector<Json> alarm_rows(const std::string& dev);
    std::string version() const;
private:
    // 统一写事务入口，负责锁等待、BEGIN/COMMIT、失败回滚以及测试故障注入。
    void write(const std::string& operation, const std::function<void()>& body);
    void finish_in_transaction(const QueryTask& task);
    std::int64_t insert_query(const QueryTask& task, const std::string& key);
    std::optional<Json> read_task(std::int64_t id);
    void checkpoint(const std::string& phase);
    void exec(const std::string& sql);
    std::int64_t scalar(const std::string& sql);
    std::unique_ptr<MysqlConnection> db_;
    StoreOptions options_;
    StoreShared& shared_;
    const std::atomic<bool>& cancel_;
    Time lock_end_{};
    bool crash_this_ = false;
};
} // namespace batt
