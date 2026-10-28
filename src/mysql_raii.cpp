#include "mysql_raii.hpp"
#include "mysql_runtime.hpp"
#include <mysql.h>
#include <atomic>
#include <charconv>
#include <climits>
#include <limits>
#include <vector>

namespace batt {
namespace {
std::atomic<bool> library_ready{false};
std::atomic<unsigned> connection_count{0};
void check(int result, const char* operation, MYSQL* db) {
    if (result) throw StorageError(operation, mysql_errno(db));
}
void statement_check(int result, const char* operation, MYSQL_STMT* statement) {
    if (result) throw StorageError(operation, mysql_stmt_errno(statement));
}
struct MysqlCloser { void operator()(MYSQL* db) const { if (db) mysql_close(db); } };
struct StatementCloser { void operator()(MYSQL_STMT* value) const { if (value) mysql_stmt_close(value); } };
struct ResultCloser { void operator()(MYSQL_RES* value) const { if (value) mysql_free_result(value); } };
}
MysqlLibrary::MysqlLibrary() {
    bool expected = false;
    if (!library_ready.compare_exchange_strong(expected, true)) throw StorageError("mysql_library_already_initialized");
    if (mysql_library_init(0, nullptr, nullptr)) {
        library_ready = false; throw StorageError("mysql_library_init_failed");
    }
}
MysqlLibrary::~MysqlLibrary() {
    if (connection_count.load()) std::terminate(); // Ordering bug, never close a live library under a connection.
    mysql_library_end(); library_ready = false;
}
MysqlThread::MysqlThread() {
    if (!library_ready.load() || mysql_thread_init()) throw StorageError("mysql_thread_init_failed");
}
MysqlThread::~MysqlThread() { mysql_thread_end(); }
const char* mysql_client_version() { return mysql_get_client_info(); }

struct MysqlConnection::Impl { std::unique_ptr<MYSQL, MysqlCloser> db; };
MysqlConnection::MysqlConnection(const DbConfig& options) : impl_(std::make_unique<Impl>()) {
    if (!library_ready.load()) throw StorageError("mysql_library_not_initialized");
    if (std::string(mysql_client_version()).rfind("8.4.", 0) != 0) throw StorageError("Oracle_MySQL_8_4_client_required");
    impl_->db.reset(mysql_init(nullptr));
    auto* db = impl_->db.get();
    if (!db) throw StorageError("mysql_connection_allocation_failed");
    check(mysql_options(db, MYSQL_OPT_CONNECT_TIMEOUT, &options.connect_timeout), "set_connect_timeout", db);
    check(mysql_options(db, MYSQL_OPT_READ_TIMEOUT, &options.read_timeout), "set_read_timeout", db);
    check(mysql_options(db, MYSQL_OPT_WRITE_TIMEOUT, &options.write_timeout), "set_write_timeout", db);
    const auto protocol = MYSQL_PROTOCOL_TCP;
    check(mysql_options(db, MYSQL_OPT_PROTOCOL, &protocol), "set_tcp_protocol", db);
    // CLIENT_FOUND_ROWS gives UPDATE matched-row semantics even for unchanged values.
    if (!mysql_real_connect(db, options.host.c_str(), options.user.c_str(), options.password.c_str(),
        options.database.c_str(), options.port, nullptr, CLIENT_FOUND_ROWS)) throw StorageError("mysql_connect", mysql_errno(db));
    if (std::string(mysql_get_server_info(db)).rfind("8.4.", 0) != 0) throw StorageError("MySQL_8_4_server_required");
    check(mysql_set_character_set(db, "utf8mb4"), "set_utf8mb4", db);
    exec("SET SESSION sql_mode='STRICT_ALL_TABLES,NO_ENGINE_SUBSTITUTION'");
    exec("SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED");
    exec("SET SESSION innodb_lock_wait_timeout=" + std::to_string(options.lock_wait_timeout));
    {
        MysqlStatement vendor(*this, "SELECT @@version_comment,@@innodb_flush_log_at_trx_commit");
        if (!vendor.row() || vendor.text(0).find("MySQL") == std::string::npos || vendor.number(1) != 1)
            throw StorageError("required_Oracle_MySQL_durability_configuration_unavailable");
    }
    ++connection_count;
}
MysqlConnection::~MysqlConnection() { impl_->db.reset(); --connection_count; }
void MysqlConnection::exec(const std::string& sql) {
    auto* db = impl_->db.get();
    if (sql.size() > ULONG_MAX) throw StorageError("SQL_length_overflow");
    check(mysql_real_query(db, sql.data(), static_cast<unsigned long>(sql.size())), "mysql_execute", db);
    // Discard any administrative result deliberately, without leaking it into the next command.
    if (mysql_field_count(db)) {
        std::unique_ptr<MYSQL_RES, ResultCloser> result(mysql_store_result(db));
        if (!result) throw StorageError("mysql_store_result", mysql_errno(db));
    }
}
void MysqlConnection::commit() {
    auto* db = impl_->db.get();
    if (mysql_commit(db)) throw StorageError("mysql_commit", mysql_errno(db), true);
}
void MysqlConnection::rollback() {
    auto* db = impl_->db.get();
    if (mysql_rollback(db)) throw StorageError("mysql_rollback", mysql_errno(db));
}
void MysqlConnection::rollback_best_effort() noexcept { mysql_rollback(impl_->db.get()); }

struct MysqlStatement::Impl {
    enum class Kind { unset, null, bytes, integer };
    struct Parameter { Kind kind = Kind::unset; std::string bytes; std::int64_t integer = 0; unsigned long length = 0; };
    struct Column { std::string bytes; unsigned long length = 0; bool null = false, truncated = false; };
    std::unique_ptr<MYSQL_STMT, StatementCloser> statement;
    std::vector<Parameter> parameters;
    std::vector<MYSQL_BIND> bindings, outputs;
    std::vector<Column> columns;
    bool executed = false, on_row = false;
    Parameter& parameter(int n) {
        if (executed || n < 1 || static_cast<std::size_t>(n) > parameters.size()) throw StorageError("invalid_bind_parameter");
        return parameters[static_cast<std::size_t>(n - 1)];
    }
    const Column& column(int n) const {
        if (!on_row || n < 0 || static_cast<std::size_t>(n) >= columns.size()) throw StorageError("invalid_result_column");
        return columns[static_cast<std::size_t>(n)];
    }
    void execute() {
        if (executed) return;
        auto* value = statement.get();
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            auto& parameter = parameters[index]; auto& binding = bindings[index];
            if (parameter.kind == Kind::unset) throw StorageError("unbound_SQL_parameter");
            if (parameter.kind == Kind::null) binding.buffer_type = MYSQL_TYPE_NULL;
            else if (parameter.kind == Kind::integer) {
                binding.buffer_type = MYSQL_TYPE_LONGLONG; binding.buffer = &parameter.integer;
            } else {
                if (parameter.bytes.size() > ULONG_MAX) throw StorageError("SQL_parameter_length_overflow");
                parameter.length = static_cast<unsigned long>(parameter.bytes.size());
                binding.buffer_type = MYSQL_TYPE_BLOB; binding.buffer = parameter.bytes.data();
                binding.buffer_length = parameter.length; binding.length = &parameter.length;
            }
        }
        if (!bindings.empty()) statement_check(mysql_stmt_bind_param(value, bindings.data()), "mysql_bind_parameters", value);
        statement_check(mysql_stmt_execute(value), "mysql_statement_execute", value);
        executed = true;
        const auto count = mysql_stmt_field_count(value);
        if (!count) return;
        columns.resize(count); outputs.resize(count);
        for (std::size_t index = 0; index < count; ++index) {
            outputs[index].buffer_type = MYSQL_TYPE_STRING;
            outputs[index].length = &columns[index].length; outputs[index].is_null = &columns[index].null;
            outputs[index].error = &columns[index].truncated;
        }
        statement_check(mysql_stmt_bind_result(value, outputs.data()), "mysql_bind_results", value);
        // Buffer before fetching: nested statements on the same owning connection are safe.
        statement_check(mysql_stmt_store_result(value), "mysql_buffer_results", value);
    }
};
MysqlStatement::MysqlStatement(MysqlConnection& connection, const std::string& sql) : impl_(std::make_unique<Impl>()) {
    auto* db = connection.impl_->db.get();
    impl_->statement.reset(mysql_stmt_init(db));
    auto* value = impl_->statement.get();
    if (!value) throw StorageError("mysql_statement_allocation", mysql_errno(db));
    if (sql.size() > ULONG_MAX) throw StorageError("SQL_length_overflow");
    statement_check(mysql_stmt_prepare(value, sql.data(), static_cast<unsigned long>(sql.size())), "mysql_prepare", value);
    const auto count = mysql_stmt_param_count(value);
    impl_->parameters.resize(count); impl_->bindings.resize(count);
}
MysqlStatement::~MysqlStatement() = default;
void MysqlStatement::text(int n, const std::string& value) { auto& item = impl_->parameter(n); item.kind = Impl::Kind::bytes; item.bytes = value; }
void MysqlStatement::number(int n, std::int64_t value) { auto& item = impl_->parameter(n); item.kind = Impl::Kind::integer; item.integer = value; }
void MysqlStatement::null(int n) { impl_->parameter(n).kind = Impl::Kind::null; }
bool MysqlStatement::row() {
    impl_->execute(); impl_->on_row = false;
    if (impl_->columns.empty()) return false;
    auto* value = impl_->statement.get();
    const int result = mysql_stmt_fetch(value);
    if (result == MYSQL_NO_DATA) return false;
    if (result != 0 && result != MYSQL_DATA_TRUNCATED) throw StorageError("mysql_fetch", mysql_stmt_errno(value));
    for (std::size_t index = 0; index < impl_->columns.size(); ++index) {
        auto& column = impl_->columns[index]; column.bytes.clear();
        if (column.null) continue;
        if (column.length > 4 * 1024 * 1024) throw StorageError("database_result_field_limit_exceeded");
        column.bytes.resize(column.length);
        MYSQL_BIND binding{}; binding.buffer_type = MYSQL_TYPE_STRING;
        binding.buffer = column.bytes.data(); binding.buffer_length = column.length;
        statement_check(mysql_stmt_fetch_column(value, &binding, static_cast<unsigned>(index), 0), "mysql_fetch_column", value);
    }
    impl_->on_row = true; return true;
}
void MysqlStatement::done() { if (row()) throw StorageError("unexpected_SQL_result_row"); }
std::string MysqlStatement::text(int n) const { return impl_->column(n).bytes; }
bool MysqlStatement::is_null(int n) const { return impl_->column(n).null; }
std::int64_t MysqlStatement::number(int n) const {
    const auto& column = impl_->column(n);
    std::int64_t result = 0;
    const auto converted = std::from_chars(column.bytes.data(), column.bytes.data() + column.bytes.size(), result);
    if (column.null || converted.ec != std::errc{} || converted.ptr != column.bytes.data() + column.bytes.size())
        throw StorageError("database_integer_result_invalid");
    return result;
}
std::uint64_t MysqlStatement::affected_rows() const {
    if (!impl_->executed) throw StorageError("SQL_not_executed");
    const auto count = mysql_stmt_affected_rows(impl_->statement.get());
    if (count == static_cast<my_ulonglong>(-1)) throw StorageError("invalid_affected_row_count");
    return count;
}
std::int64_t MysqlStatement::insert_id() const {
    if (!impl_->executed) throw StorageError("SQL_not_executed");
    const auto id = mysql_stmt_insert_id(impl_->statement.get());
    if (!id || id > static_cast<my_ulonglong>(std::numeric_limits<std::int64_t>::max())) throw StorageError("database_identifier_exhausted");
    return static_cast<std::int64_t>(id);
}
MysqlTransaction::MysqlTransaction(MysqlConnection& connection) : connection_(connection) { connection_.exec("START TRANSACTION"); }
MysqlTransaction::~MysqlTransaction() { if (active_) connection_.rollback_best_effort(); }
void MysqlTransaction::commit() { connection_.commit(); active_ = false; }
void MysqlTransaction::rollback() { connection_.rollback(); active_ = false; }
} // namespace batt
