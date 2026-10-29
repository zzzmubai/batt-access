#pragma once
namespace batt {
class MysqlConnection;
void validate_mysql_schema(MysqlConnection& connection);
}
