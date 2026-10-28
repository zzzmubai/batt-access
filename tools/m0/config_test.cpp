#include "config.hpp"
#include <iostream>
#include <sstream>

int main() {
    const std::string runtime = std::filesystem::current_path().generic_string();
    const std::string good = "[mysql]\nhost=127.0.0.1\nport=3306\ndatabase=batt_test_m0\nuser=probe\n"
        "password=local-test-secret\nruntime_dir=" + runtime + "\nconnect_timeout=2\nread_timeout=1\nwrite_timeout=1\n";
    unsigned checks = 0;
    const auto require = [&](bool condition) { ++checks; if (!condition) throw std::runtime_error("guard assertion failed"); };
    const auto rejects = [&](const std::string& value) {
        try { std::istringstream stream(value); m0::parse(stream); }
        catch (const std::runtime_error& error) {
            require(std::string(error.what()).find("local-test-secret") == std::string::npos);
            return true;
        }
        return false;
    };
    const auto change = [&](const std::string& from, const std::string& to) {
        auto value = good; value.replace(value.find(from), from.size(), to); return value;
    };
    try {
        std::istringstream stream(good);
        const auto config = m0::parse(stream);
        require(config.port == 3306 && config.database == "batt_test_m0" && config.password == "local-test-secret");
        for (const auto& value : std::initializer_list<std::string>{"access", "batt_test_", "batt_test_a;DROP", "batt_test_中文", "batt_test_a b", std::string(65, 'x')})
            require(rejects(change("batt_test_m0", value)));
        for (const auto& value : {"0", "65536", "-1", "3306junk", "999999999999999999999999999999999"})
            require(rejects(change("port=3306", std::string("port=") + value)));
        require(rejects(change("host=127.0.0.1", "host=192.168.1.2")));
        require(rejects(change("runtime_dir=" + runtime, "runtime_dir=relative")));
        require(rejects(change("read_timeout=1", "read_timeout=31")));
        require(rejects(change("local-test-secret", "REPLACE_ME")));
        require(rejects(change("user=probe\n", "")));
        require(rejects(good + "password=other\n"));
        require(rejects(good + "[mysql]\n"));
        require(rejects(good + "unknown=local-test-secret\n"));
        bool relative_rejected = false;
        try { m0::load("relative.ini"); } catch (const std::runtime_error&) { relative_rejected = true; }
        require(relative_rejected);
        std::cout << "M0_CONFIG_PASS checks=" << checks << '\n';
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
