#include "mysql_schema.hpp"
#include "mysql_raii.hpp"
#include "mysql_schema_manifest.hpp"
#include "protocol.hpp"
#include <cctype>
#include <set>

namespace batt {
namespace {
std::string normalize(std::string value) {
    std::string result;
    for (const unsigned char ch : value)
        if (!std::isspace(ch) && ch != '`' && ch != '(' && ch != ')') result += static_cast<char>(std::tolower(ch));
    for (const auto& replacement : {std::pair<std::string, std::string>{"_binary", ""}, {"\\'", "'"}, {"octet_length", "length"}}) {
        std::size_t at = 0;
        while ((at = result.find(replacement.first, at)) != std::string::npos) {
            result.replace(at, replacement.first.size(), replacement.second); at += replacement.second.size();
        }
    }
    return result;
}
void require(bool value, const char* reason) { if (!value) throw StorageError(reason); }
}
void validate_mysql_schema(MysqlConnection& connection) {
    const auto expected = Json::parse(mysql_schema_manifest_json);
    {
        MysqlStatement tables(connection, "SELECT TABLE_NAME,ENGINE,TABLE_COLLATION FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() ORDER BY TABLE_NAME");
        auto names = Json::array();
        while (tables.row()) {
            names.push_back(tables.text(0));
            require(tables.text(1) == "InnoDB" && tables.text(2) == "utf8mb4_bin", "mysql_schema_engine_or_collation_mismatch");
        }
        require(names == expected.at("tables"), "mysql_schema_table_set_mismatch");
    }
    for (const auto& table : expected.at("columns").items()) {
        MysqlStatement columns(connection, "SELECT COLUMN_NAME,COLUMN_TYPE,IS_NULLABLE,EXTRA,GENERATION_EXPRESSION FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME=?");
        columns.text(1, table.key());
        std::set<std::string> found;
        while (columns.row()) {
            const auto name = columns.text(0); found.insert(name);
            require(table.value().contains(name), "mysql_schema_unexpected_column");
            auto type = table.value().at(name).get<std::string>();
            const bool nullable = !type.empty() && type.back() == '?';
            if (nullable) type.pop_back();
            require(columns.text(1) == type && (columns.text(2) == "YES") == nullable, "mysql_schema_column_type_or_nullability_mismatch");
            if (name == "id" || name == "txn_no") require(columns.text(3).find("auto_increment") != std::string::npos, "mysql_schema_auto_increment_missing");
            if (name == "open_guard" || name == "pending_guard") {
                const auto status = name == "open_guard" ? "open" : "pending";
                require(columns.text(3) == "STORED GENERATED" && normalize(columns.text(4)) ==
                    std::string("casewhenstatus='") + status + "'then1elsenullend", "mysql_schema_generated_guard_mismatch");
            }
        }
        require(found.size() == table.value().size(), "mysql_schema_column_missing");
        MysqlStatement indexes(connection, "SELECT INDEX_NAME,NON_UNIQUE,COLUMN_NAME FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME=? ORDER BY INDEX_NAME,SEQ_IN_INDEX");
        indexes.text(1, table.key());
        auto actual = Json::object();
        while (indexes.row()) {
            const auto name = indexes.text(0);
            if (!actual.contains(name)) actual[name] = Json::array({indexes.number(1), Json::array()});
            actual[name][1].push_back(indexes.text(2));
        }
        require(actual == expected.at("indexes").at(table.key()), "mysql_schema_index_mismatch");
    }
    {
        MysqlStatement checks(connection, "SELECT t.CONSTRAINT_NAME,t.ENFORCED,c.CHECK_CLAUSE FROM information_schema.TABLE_CONSTRAINTS t JOIN information_schema.CHECK_CONSTRAINTS c ON t.CONSTRAINT_SCHEMA=c.CONSTRAINT_SCHEMA AND t.CONSTRAINT_NAME=c.CONSTRAINT_NAME WHERE t.CONSTRAINT_SCHEMA=DATABASE() AND t.CONSTRAINT_TYPE='CHECK'");
        auto actual = Json::object();
        while (checks.row()) {
            require(checks.text(1) == "YES", "mysql_schema_unenforced_check"); actual[checks.text(0)] = normalize(checks.text(2));
        }
        require(actual == expected.at("checks"), "mysql_schema_check_expression_mismatch");
    }
    {
        MysqlStatement keys(connection, "SELECT CONSTRAINT_NAME,TABLE_NAME,COLUMN_NAME,REFERENCED_TABLE_NAME,REFERENCED_COLUMN_NAME FROM information_schema.KEY_COLUMN_USAGE WHERE CONSTRAINT_SCHEMA=DATABASE() AND REFERENCED_TABLE_NAME IS NOT NULL ORDER BY CONSTRAINT_NAME,ORDINAL_POSITION");
        auto actual = Json::array();
        while (keys.row()) actual.push_back({keys.text(0), keys.text(1), keys.text(2), keys.text(3), keys.text(4)});
        const Json required = {{"alarm_device_fk", "alarm", "dev_id", "device", "dev_id"},
                               {"query_device_fk", "query_task", "dev_id", "device", "dev_id"}};
        require(actual == required, "mysql_schema_foreign_key_mismatch");
        MysqlStatement actions(connection, "SELECT UPDATE_RULE,DELETE_RULE FROM information_schema.REFERENTIAL_CONSTRAINTS WHERE CONSTRAINT_SCHEMA=DATABASE()");
        while (actions.row()) for (int column = 0; column < 2; ++column)
            require(actions.text(column) == "NO ACTION" || actions.text(column) == "RESTRICT", "mysql_schema_foreign_key_action_mismatch");
    }
    {
        MysqlStatement charset(connection, "SELECT CHARACTER_SET_NAME,COLLATION_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='query_task' AND COLUMN_NAME='response_json'");
        require(charset.row() && charset.text(0) == "utf8mb4" && charset.text(1) == "utf8mb4_bin", "mysql_schema_response_charset_mismatch");
        MysqlStatement meta(connection, "SELECT singleton,application_id,schema_version,initialized FROM schema_meta");
        require(meta.row() && meta.number(0) == 1 && meta.text(1) == "batt-access" && meta.number(2) == 1 && meta.number(3) == 1,
            "unsupported_or_incomplete_mysql_schema");
        require(!meta.row(), "mysql_schema_metadata_not_singleton");
    }
}
} // namespace batt
