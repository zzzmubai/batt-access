#include "db_config.hpp"
#include "mysql_raii.hpp"
#include "mysql_runtime.hpp"
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>

namespace {
unsigned checks = 0;
void require(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
std::int64_t count(batt::MysqlConnection& connection, const std::string& id) {
    batt::MysqlStatement statement(connection, "SELECT COUNT(*) FROM device WHERE dev_id=?");
    statement.text(1, id); require(statement.row(), "count row missing"); return statement.number(0);
}
void insert(batt::MysqlConnection& connection, const std::string& id, const std::string& imsi = "SIM") {
    batt::MysqlStatement statement(connection, "INSERT INTO device(dev_id,imsi,first_seen_ms,last_login_ms) VALUES(?,?,?,?)");
    statement.text(1, id); statement.text(2, imsi); statement.number(3, 1); statement.number(4, 2); statement.done();
    require(statement.affected_rows() == 1, "insert affected row count");
}
void resources(const batt::DbConfig& config) {
    batt::MysqlConnection connection(config), observer(config);
    const std::string id("A\0z ", 4);
    const char raw_payload[] = "' % \\ 汉字 \0 end";
    const std::string payload(raw_payload, sizeof(raw_payload) - 1);
    {
        batt::MysqlTransaction transaction(connection);
        insert(connection, id, payload);
        require(count(connection, id) == 1 && count(observer, id) == 0, "uncommitted data escaped");
        transaction.commit();
    }
    require(count(observer, id) == 1, "committed data invisible");
    {
        batt::MysqlStatement statement(connection, "SELECT imsi,NULL,?,? FROM device WHERE dev_id=?");
        statement.number(1, 9007199254740993LL); statement.number(2, std::numeric_limits<std::int64_t>::min()); statement.text(3, id);
        require(statement.row() && statement.text(0) == payload && statement.is_null(1), "binary length/null lost");
        require(statement.number(2) == 9007199254740993LL && statement.number(3) == std::numeric_limits<std::int64_t>::min(), "signed integer precision");
        require(!statement.row(), "unexpected second row");
    }
    {
        batt::MysqlStatement unchanged(connection, "UPDATE device SET last_login_ms=2 WHERE dev_id=?");
        unchanged.text(1, id); unchanged.done();
        require(unchanged.affected_rows() == 1, "unchanged UPDATE lost matched-row semantics");
    }
    {
        batt::MysqlTransaction transaction(connection); insert(connection, "rollback-explicit"); transaction.rollback();
    }
    require(count(observer, "rollback-explicit") == 0, "explicit rollback persisted");
    struct Unwind {};
    try {
        batt::MysqlTransaction transaction(connection); insert(connection, "rollback-unwind"); throw Unwind{};
    } catch (const Unwind&) {}
    require(count(observer, "rollback-unwind") == 0, "unwind rollback persisted");
    const std::string injection = "X'); DROP TABLE device;--";
    { batt::MysqlTransaction transaction(connection); insert(connection, injection); transaction.commit(); }
    require(count(observer, injection) == 1, "parameter interpreted as SQL");
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        try { batt::MysqlStatement invalid(connection, "SELECT ? FROM __missing_table_secret"); require(false, "missing table accepted"); }
        catch (const batt::StorageError& error) {
            require(error.code == 1146 && std::string(error.what()).find("secret") == std::string::npos, "prepare classification/redaction");
        }
    }
    try {
        batt::MysqlTransaction transaction(connection); insert(connection, id);
        require(false, "duplicate accepted");
    } catch (const batt::StorageError& error) { require(error.code == 1062, "duplicate error code lost"); }
    require(count(connection, id) == 1, "failed statement corrupted connection");
    {
        batt::MysqlStatement insert_task(connection, "INSERT INTO query_task(dev_id,status,created_ms,finished_ms,reason,response_json) VALUES(?,'link_lost',1,2,?,?)");
        insert_task.text(1, id); insert_task.text(2, ""); insert_task.null(3); insert_task.done();
        require(insert_task.insert_id() > 0, "insert id unavailable");
        batt::MysqlStatement task(connection, "SELECT reason,connection_id,response_json,client_request_id FROM query_task WHERE txn_no=?");
        task.number(1, insert_task.insert_id());
        require(task.row() && !task.is_null(0) && task.text(0).empty() && task.is_null(1) && task.is_null(2) && task.is_null(3), "empty string and NULL conflated");
    }
    // The outer result remains valid while another prepared statement uses the connection.
    batt::MysqlStatement outer(connection, "SELECT dev_id FROM device ORDER BY dev_id");
    unsigned rows = 0;
    while (outer.row()) { require(count(connection, outer.text(0)) == 1, "nested result lifetime"); ++rows; }
    require(rows == 2, "unexpected resource-test device count");
}
}
int main() {
    try {
        batt::MysqlLibrary library;
        const auto* path = std::getenv("BATT_TEST_DB_CONFIG"); require(path != nullptr, "test configuration missing");
        const auto config = batt::load_db_config(path);
        auto denied = config; denied.password = "invalid-secret-sentinel";
        try { batt::MysqlConnection invalid(denied); require(false, "invalid credentials accepted"); }
        catch (const batt::StorageError& error) {
            require(error.code == 1045 && std::string(error.what()).find(denied.password) == std::string::npos &&
                std::string(error.what()).find(config.user) == std::string::npos, "connection error classification/redaction");
        }
        resources(config);
        std::exception_ptr thread_error;
        std::thread worker([&] {
            try { batt::MysqlThread runtime; batt::MysqlConnection own(config); require(count(own, "rollback-unwind") == 0, "worker connection state"); }
            catch (...) { thread_error = std::current_exception(); }
        });
        worker.join(); if (thread_error) std::rethrow_exception(thread_error);
        std::cout << "M2_MYSQL_RAII_PASS checks=" << checks << '\n';
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
