#include "db_config.hpp"
#include "storage_error.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {
unsigned checks = 0;
void require(bool value) { ++checks; if (!value) throw std::runtime_error("configuration guard failed"); }
const std::string example = "[mysql]\nhost=127.0.0.1\nport=3306\ndatabase=batt_test_unit\nuser=test\n"
    "password=sentinel#%=secret\nruntime_dir=/tmp\nconnect_timeout=2\nread_timeout=1\nwrite_timeout=1\n";
bool rejected(const std::string& value) {
    try { std::istringstream input(value); batt::parse_db_config(input); }
    catch (const batt::StorageError& error) {
        require(std::string(error.what()).find("sentinel#%=secret") == std::string::npos); return true;
    }
    return false;
}
std::string replace(const std::string& from, const std::string& to) {
    auto result = example; result.replace(result.find(from), from.size(), to); return result;
}
struct Fixture {
    std::string directory, config, alias;
    Fixture() {
        char path[] = "/tmp/batt-config-XXXXXX";
        const auto* result = ::mkdtemp(path); if (!result) throw std::runtime_error("mkdtemp failed");
        directory = result; config = directory + "/mysql.ini"; alias = directory + "/alias";
    }
    ~Fixture() { ::unlink(config.c_str()); ::unlink(alias.c_str()); ::rmdir(directory.c_str()); }
};
}
int main() {
    try {
        std::istringstream input(example);
        const auto good = batt::parse_db_config(input);
        require(good.password == "sentinel#%=secret" && good.lock_wait_timeout == 1 && good.read_timeout == 1);
        for (const auto& name : {"UpperCase", "a;DROP", "a b", "a-b", "9first", "中文"})
            require(rejected(replace("batt_test_unit", name)));
        require(rejected(replace("batt_test_unit", std::string(65, 'a'))));
        for (const auto& port : {"0", "65536", "-1", "3306junk", "999999999999999999999999999999"})
            require(rejected(replace("port=3306", std::string("port=") + port)));
        require(rejected(replace("host=127.0.0.1", "host=localhost")));
        require(rejected(replace("host=127.0.0.1", "host=192.168.1.1")));
        require(rejected(replace("runtime_dir=/tmp", "runtime_dir=relative")));
        require(rejected(replace("connect_timeout=2", "connect_timeout=31")));
        require(rejected(example + "lock_wait_timeout=0\n"));
        require(rejected(example + "password=another\n"));
        require(rejected(example + "unknown=sentinel#%=secret\n"));
        require(rejected(example + "[mysql]\n"));
        require(rejected(replace("user=test\n", "")));
        require(rejected(replace("sentinel#%=secret", "REPLACE_ME")));
        Fixture fixture;
        require(::symlink(fixture.directory.c_str(), fixture.alias.c_str()) == 0);
        {
            std::ofstream output(fixture.config); output << replace("runtime_dir=/tmp", "runtime_dir=" + fixture.alias);
        }
        require(::chmod(fixture.config.c_str(), 0600) == 0);
        require(batt::load_db_config(fixture.config).runtime_dir == fixture.directory);
        require(::chmod(fixture.config.c_str(), 0644) == 0); // Only the dummy sentinel config, never real credentials.
        bool unsafe_rejected = false;
        try { batt::load_db_config(fixture.config); } catch (const batt::StorageError&) { unsafe_rejected = true; }
        require(unsafe_rejected);
        bool relative_rejected = false;
        try { batt::load_db_config("relative.ini"); } catch (const batt::StorageError&) { relative_rejected = true; }
        require(relative_rejected);
        std::cout << "M2_CONFIG_PASS checks=" << checks << '\n';
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
