// M0-only configuration: no connection is attempted before validation succeeds.
#pragma once
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>

namespace m0 {
struct Config {
    std::string host, database, user, password, runtime_dir;
    unsigned port = 3306, connect_timeout = 2, read_timeout = 1, write_timeout = 1;
};
inline std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
inline Config parse(std::istream& input) {
    std::map<std::string, std::string> values;
    bool section = false;
    for (std::string line; std::getline(input, line);) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line == "[mysql]") {
            if (section) throw std::runtime_error("duplicate config section");
            section = true;
            continue;
        }
        const auto at = line.find('=');
        if (!section || at == std::string::npos) throw std::runtime_error("invalid config line");
        const auto key = trim(line.substr(0, at));
        const auto value = trim(line.substr(at + 1));
        if (!values.emplace(key, value).second) throw std::runtime_error("duplicate config key");
    }
    if (input.bad()) throw std::runtime_error("config read failed");
    const auto get = [&](const std::string& key) {
        const auto it = values.find(key);
        if (it == values.end() || it->second.empty()) throw std::runtime_error("missing config field: " + key);
        return it->second;
    };
    const auto number = [&](const std::string& key, unsigned maximum) {
        const auto value = get(key);
        unsigned result = 0;
        for (const unsigned char ch : value) {
            if (ch < '0' || ch > '9' || result > (maximum - (ch - '0')) / 10)
                throw std::runtime_error("invalid config number: " + key);
            result = result * 10 + (ch - '0');
        }
        if (result == 0 || result > maximum) throw std::runtime_error("config number out of range: " + key);
        return result;
    };
    for (const auto& pair : values) {
        if (pair.first != "host" && pair.first != "port" && pair.first != "database" && pair.first != "user" &&
            pair.first != "password" && pair.first != "runtime_dir" && pair.first != "connect_timeout" &&
            pair.first != "read_timeout" && pair.first != "write_timeout")
            throw std::runtime_error("unknown config key");
    }
    Config result;
    result.host = get("host"); result.database = get("database");
    result.user = get("user"); result.password = get("password"); result.runtime_dir = get("runtime_dir");
    if (result.host != "127.0.0.1") throw std::runtime_error("M0 requires loopback MySQL");
    if (result.database.size() > 64 || result.database.rfind("batt_test_", 0) != 0 || result.database.size() <= 10)
        throw std::runtime_error("M0 requires a batt_test_ database");
    for (const unsigned char ch : result.database)
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_'))
            throw std::runtime_error("database must be an ASCII identifier");
    if (!std::filesystem::path(result.runtime_dir).is_absolute()) throw std::runtime_error("runtime_dir must be absolute");
    if (result.password == "REPLACE_ME") throw std::runtime_error("replace example credentials locally");
    result.port = number("port", 65535);
    result.connect_timeout = number("connect_timeout", 30);
    result.read_timeout = number("read_timeout", 30);
    result.write_timeout = number("write_timeout", 30);
    return result;
}
inline Config load(const std::string& filename) {
    if (!std::filesystem::path(filename).is_absolute()) throw std::runtime_error("config path must be absolute");
    std::ifstream input(filename);
    if (!input) throw std::runtime_error("config cannot be opened");
    return parse(input);
}
} // namespace m0
