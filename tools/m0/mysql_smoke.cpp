// Admission program only; not the V3 Store. It creates only a session-local table.
#include "config.hpp"
#include <mysql.h>
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace {
void check(bool ok, const char* operation, unsigned code = 0) {
    if (!ok) throw std::runtime_error(std::string(operation) + " failed; mysql_code=" + std::to_string(code));
}
struct Library {
    Library() { check(mysql_library_init(0, nullptr, nullptr) == 0, "library_init"); }
    ~Library() { mysql_library_end(); }
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
};
using Connection = std::unique_ptr<MYSQL, decltype(&mysql_close)>;
using Result = std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>;
struct Statement {
    MYSQL_STMT* value;
    Statement(MYSQL* db, const char* sql) : value(mysql_stmt_init(db)) {
        check(value != nullptr, "stmt_init");
        if (mysql_stmt_prepare(value, sql, static_cast<unsigned long>(std::strlen(sql)))) {
            const auto code = mysql_stmt_errno(value); mysql_stmt_close(value); value = nullptr;
            check(false, "stmt_prepare", code);
        }
    }
    ~Statement() { if (value) mysql_stmt_close(value); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    void execute() { const int rc = mysql_stmt_execute(value); check(rc == 0, "stmt_execute", mysql_stmt_errno(value)); }
};
void sql(MYSQL* db, const char* text) {
    const int rc = mysql_query(db, text); check(rc == 0, "query", mysql_errno(db));
}
std::string scalar(MYSQL* db, const char* text) {
    sql(db, text); Result result(mysql_store_result(db), mysql_free_result);
    check(result != nullptr, "store_result", mysql_errno(db));
    const auto row = mysql_fetch_row(result.get()); check(row && row[0], "scalar");
    return row[0];
}
void insert(MYSQL* db, long long id, const std::string& bytes) {
    Statement statement(db, "INSERT INTO batt_m0_probe(id,bytes,optional) VALUES(?,?,?)");
    std::array<MYSQL_BIND, 3> parameters{};
    auto length = static_cast<unsigned long>(bytes.size());
    parameters[0].buffer_type = MYSQL_TYPE_LONGLONG; parameters[0].buffer = &id;
    parameters[1].buffer_type = MYSQL_TYPE_BLOB; parameters[1].buffer = const_cast<char*>(bytes.data());
    parameters[1].buffer_length = length; parameters[1].length = &length;
    parameters[2].buffer_type = MYSQL_TYPE_NULL;
    const int rc = mysql_stmt_bind_param(statement.value, parameters.data());
    check(rc == 0, "bind_param", mysql_stmt_errno(statement.value));
    statement.execute();
}
void read(MYSQL* db, long long expected_id, const std::string& expected_bytes) {
    Statement statement(db, "SELECT id,bytes,optional FROM batt_m0_probe");
    statement.execute();
    long long id = 0; char bytes[64]{}; unsigned long length = 0; bool null_optional = false;
    std::array<MYSQL_BIND, 3> outputs{};
    outputs[0].buffer_type = MYSQL_TYPE_LONGLONG; outputs[0].buffer = &id;
    outputs[1].buffer_type = MYSQL_TYPE_BLOB; outputs[1].buffer = bytes;
    outputs[1].buffer_length = sizeof(bytes); outputs[1].length = &length;
    outputs[2].buffer_type = MYSQL_TYPE_STRING; outputs[2].is_null = &null_optional;
    const int bound = mysql_stmt_bind_result(statement.value, outputs.data());
    check(bound == 0, "bind_result", mysql_stmt_errno(statement.value));
    const int fetched = mysql_stmt_fetch(statement.value);
    check(fetched == 0, "fetch", mysql_stmt_errno(statement.value));
    check(id == expected_id && length == expected_bytes.size() && std::string(bytes, length) == expected_bytes && null_optional,
        "integer_binary_null_roundtrip");
    check(mysql_stmt_fetch(statement.value) == MYSQL_NO_DATA, "single_row");
}
struct Transaction {
    MYSQL* db; bool active = true;
    explicit Transaction(MYSQL* connection) : db(connection) { sql(db, "START TRANSACTION"); }
    ~Transaction() { if (active) mysql_rollback(db); }
    void rollback() { const int rc = mysql_rollback(db); check(rc == 0, "rollback", mysql_errno(db)); active = false; }
    void commit() {
        const int rc = mysql_commit(db);
        // Failure may mean an unknown commit outcome. Never replay this transaction.
        check(rc == 0, "commit_outcome_may_be_unknown", mysql_errno(db)); active = false;
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
};
} // namespace
int main(int argc, char** argv) {
    try {
        check(argc == 2, "usage: mysql_smoke /absolute/private-config.ini");
        struct stat info{};
        check(::stat(argv[1], &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == ::geteuid() &&
            (info.st_mode & 0077) == 0, "config_owner_and_private_permissions");
        const auto config = m0::load(argv[1]);
        Library library; // Declared first, released after statements and connections.
        const std::string client = mysql_get_client_info();
        check(client.rfind("8.4.", 0) == 0, "Oracle_MySQL_8_4_client_required");
        Connection connection(mysql_init(nullptr), mysql_close); check(connection != nullptr, "mysql_init");
        MYSQL* db = connection.get();
        check(mysql_options(db, MYSQL_OPT_CONNECT_TIMEOUT, &config.connect_timeout) == 0, "connect_timeout");
        check(mysql_options(db, MYSQL_OPT_READ_TIMEOUT, &config.read_timeout) == 0, "read_timeout");
        check(mysql_options(db, MYSQL_OPT_WRITE_TIMEOUT, &config.write_timeout) == 0, "write_timeout");
        const auto connected = mysql_real_connect(db, config.host.c_str(), config.user.c_str(), config.password.c_str(),
            config.database.c_str(), config.port, nullptr, 0);
        check(connected != nullptr, "connect", mysql_errno(db));
        const std::string server = mysql_get_server_info(db);
        check(server.rfind("8.4.", 0) == 0, "MySQL_8_4_server_required");
        check(scalar(db, "SELECT @@version_comment").find("MySQL") != std::string::npos, "Oracle_server_required");
        check(mysql_set_character_set(db, "utf8mb4") == 0, "utf8mb4", mysql_errno(db));
        sql(db, "SET SESSION sql_mode='STRICT_ALL_TABLES,NO_ENGINE_SUBSTITUTION'");
        sql(db, "SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED");
        sql(db, "SET SESSION innodb_lock_wait_timeout=1");
        check(scalar(db, "SELECT @@innodb_flush_log_at_trx_commit") == "1", "durability_setting_required");
        std::cout << "client=" << client << " server=" << server
                  << " isolation=" << scalar(db, "SELECT @@transaction_isolation")
                  << " flush=" << scalar(db, "SELECT @@innodb_flush_log_at_trx_commit")
                  << " binlog=" << scalar(db, "SELECT @@log_bin")
                  << " sync_binlog=" << scalar(db, "SELECT @@sync_binlog") << '\n';
        sql(db, "CREATE TEMPORARY TABLE batt_m0_probe(id BIGINT PRIMARY KEY,bytes VARBINARY(64) NOT NULL,optional LONGTEXT NULL) ENGINE=InnoDB");
        const std::string bytes("A\0z ", 4);
        constexpr long long large = 9007199254740993LL;
        {
            Transaction transaction(db); insert(db, large, bytes); read(db, large, bytes); transaction.rollback();
        }
        check(scalar(db, "SELECT COUNT(*) FROM batt_m0_probe") == "0", "rollback_visibility");
        {
            Transaction transaction(db); insert(db, large, bytes); transaction.commit();
        }
        read(db, large, bytes);
        struct ControlledUnwind {};
        try {
            Transaction transaction(db); sql(db, "DELETE FROM batt_m0_probe");
            throw ControlledUnwind{};
        } catch (const ControlledUnwind&) {}
        read(db, large, bytes);
        std::cout << "M0_MYSQL_PASS parameter_binding integer64 binary_length null commit explicit_rollback raii_rollback\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
