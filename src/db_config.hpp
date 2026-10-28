#pragma once
#include <istream>
#include <string>

namespace batt {
struct DbConfig {
    std::string host, database, user, password, runtime_dir;
    unsigned port = 3306, connect_timeout = 2, read_timeout = 1, write_timeout = 1, lock_wait_timeout = 1;
};
DbConfig parse_db_config(std::istream& input);
DbConfig load_db_config(const std::string& absolute_filename);
} // namespace batt
