#include "db_config.hpp"
#include "storage_error.hpp"
#include <filesystem>
#include <fstream>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

namespace batt {
namespace {
std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? "" : value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
}
DbConfig parse_db_config(std::istream& input) {
    std::map<std::string, std::string> values;
    bool section = false;
    for (std::string line; std::getline(input, line);) {
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        if (line == "[mysql]") {
            if (section) throw StorageError("duplicate database section");
            section = true; continue;
        }
        const auto at = line.find('=');
        if (!section || at == std::string::npos) throw StorageError("invalid database configuration line");
        const auto key = trim(line.substr(0, at));
        if (!values.emplace(key, trim(line.substr(at + 1))).second) throw StorageError("duplicate database configuration key");
    }
    if (input.bad()) throw StorageError("database configuration read failed");
    const auto get = [&](const std::string& key) {
        const auto item = values.find(key);
        if (item == values.end() || item->second.empty()) throw StorageError("missing database field: " + key);
        return item->second;
    };
    const auto number = [&](const std::string& key, unsigned maximum) {
        const auto value = get(key); unsigned result = 0;
        for (const unsigned char ch : value) {
            if (ch < '0' || ch > '9' || result > (maximum - (ch - '0')) / 10)
                throw StorageError("invalid database number: " + key);
            result = result * 10 + (ch - '0');
        }
        if (!result || result > maximum) throw StorageError("database number out of range: " + key);
        return result;
    };
    for (const auto& item : values)
        if (item.first != "host" && item.first != "port" && item.first != "database" && item.first != "user" &&
            item.first != "password" && item.first != "runtime_dir" && item.first != "connect_timeout" &&
            item.first != "read_timeout" && item.first != "write_timeout" && item.first != "lock_wait_timeout")
            throw StorageError("unknown database configuration key");
    DbConfig result;
    result.host = get("host"); result.database = get("database"); result.user = get("user");
    result.password = get("password"); result.runtime_dir = get("runtime_dir");
    if (result.host != "127.0.0.1") throw StorageError("database requires loopback IPv4");
    if (result.database.empty() || result.database.size() > 64 || result.database.front() < 'a' || result.database.front() > 'z')
        throw StorageError("database requires a lowercase ASCII identifier");
    for (const unsigned char ch : result.database)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_'))
            throw StorageError("database requires a lowercase ASCII identifier");
    if (result.password == "REPLACE_ME") throw StorageError("replace example database credentials");
    if (!std::filesystem::path(result.runtime_dir).is_absolute()) throw StorageError("runtime_dir must be absolute");
    result.port = number("port", 65535);
    result.connect_timeout = number("connect_timeout", 30); result.read_timeout = number("read_timeout", 30);
    result.write_timeout = number("write_timeout", 30);
    if (values.count("lock_wait_timeout")) result.lock_wait_timeout = number("lock_wait_timeout", 30);
    return result;
}
DbConfig load_db_config(const std::string& filename) {
    if (!std::filesystem::path(filename).is_absolute()) throw StorageError("database configuration path must be absolute");
    struct stat info{};
    if (::stat(filename.c_str(), &info) || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 0077))
        throw StorageError("database configuration must be a private file owned by current user");
    std::ifstream input(filename);
    if (!input) throw StorageError("database configuration unavailable");
    auto result = parse_db_config(input);
    std::error_code error;
    const auto directory = std::filesystem::canonical(result.runtime_dir, error);
    if (error || !std::filesystem::is_directory(directory)) throw StorageError("fixed runtime directory unavailable");
    result.runtime_dir = directory.string();
    return result;
}
} // namespace batt
