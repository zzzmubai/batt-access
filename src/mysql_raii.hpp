#pragma once
#include "db_config.hpp"
#include "storage_error.hpp"
#include <cstdint>
#include <memory>
#include <string>

namespace batt {
class MysqlStatement;
class MysqlConnection {
public:
    explicit MysqlConnection(const DbConfig& options);
    ~MysqlConnection();
    MysqlConnection(const MysqlConnection&) = delete;
    MysqlConnection& operator=(const MysqlConnection&) = delete;
    void exec(const std::string& sql);
    void commit();
    void rollback();
    void rollback_best_effort() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class MysqlStatement;
};
class MysqlStatement {
public:
    MysqlStatement(MysqlConnection& connection, const std::string& sql);
    ~MysqlStatement();
    MysqlStatement(const MysqlStatement&) = delete;
    MysqlStatement& operator=(const MysqlStatement&) = delete;
    void text(int parameter, const std::string& value);
    void number(int parameter, std::int64_t value);
    void null(int parameter);
    bool row();
    void done();
    std::string text(int column) const;
    std::int64_t number(int column) const;
    bool is_null(int column) const;
    std::uint64_t affected_rows() const;
    std::int64_t insert_id() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class MysqlTransaction {
public:
    explicit MysqlTransaction(MysqlConnection& connection);
    ~MysqlTransaction();
    MysqlTransaction(const MysqlTransaction&) = delete;
    MysqlTransaction& operator=(const MysqlTransaction&) = delete;
    void commit();
    void rollback();
private:
    MysqlConnection& connection_;
    bool active_ = true;
};
} // namespace batt
