#include "store.hpp"
#include <iostream>
#include <unistd.h>

namespace {
unsigned checks = 0;
void check(bool yes, const char* message) { ++checks; if (!yes) throw std::runtime_error(message); }
template<class F> void rejected(F body) {
    bool threw = false;
    try { body(); } catch (const batt::StorageError&) { threw = true; }
    check(threw, "storage error was not raised");
}
struct Temp {
    std::string dir, path;
    Temp() {
        char name[] = "/tmp/batt-store-XXXXXX";
        const auto value = ::mkdtemp(name);
        if (!value) throw std::runtime_error("mkdtemp failed");
        dir = value; path = dir + "/test.db";
    }
    ~Temp() {
        for (const auto& suffix : {"", "-wal", "-shm"}) ::unlink((path + suffix).c_str());
        ::rmdir(dir.c_str());
    }
};
batt::Json hello() { return {{"msgType",110},{"devId","BT000"},{"imsi","SIM000"},{"devType",1},{"txnNo",1}}; }
batt::Json alarms(std::initializer_list<const char*> descriptions) {
    auto list = batt::Json::array();
    for (const auto desc : descriptions) list.push_back({{"id","01003001"},{"alarmDesc",desc},{"alarmFlag",1}});
    if (list.empty()) list.push_back({{"id","01003001"},{"alarmDesc","000"},{"alarmFlag",0}});
    return {{"msgType",410},{"devId","BT000"},{"txnNo",2},{"alarmList",list}};
}
void transactions() {
    Temp tmp; batt::StoreOptions options; options.path = tmp.path; options.boot_id = "boot-A";
    batt::StoreShared shared; std::atomic<bool> cancel{false};
    batt::Store store(options, shared, cancel); store.initialize();
    batt::Sessions sessions;
    auto accept = [&](const batt::Json& frame) {
        return sessions.persisted_handle(1, frame, batt::Clock::now(),
            [&](const batt::Sessions& candidate, const batt::SessionReply&) {
                if (frame["msgType"] == 110) store.login("BT000", "SIM000", {});
                else store.alarms("BT000", sessions.alarm_export("BT000"), candidate.alarm_export("BT000"));
            });
    };
    check(accept(hello()).reply.ack->at("result") == 1, "login not committed");
    check(accept(alarms({"03","06"})).reply.ack->at("result") == 1, "alarms not committed");
    check(store.alarm_rows("BT000").size() == 2, "initial alarm row count");
    accept(alarms({"03","06"}));
    check(store.alarm_rows("BT000").size() == 2, "repeat duplicated open alarm");
    accept(alarms({"06","01"}));
    auto rows = store.alarm_rows("BT000");
    unsigned open = 0;
    for (const auto& row : rows) {
        open += row["state"] == "open" ? 1U : 0U;
        check(row["first_ms"].get<std::int64_t>() > 0, "UTC alarm time missing");
        check((row["state"] == "open") == row["clear_ms"].is_null(), "alarm clear constraint");
    }
    check(rows.size() == 3 && open == 2, "set update not atomic/correct");
    check(sessions.alarm_export("BT000").size() == 2, "cleared history retained in RAM");
    auto restored = store.recover_devices();
    check(restored.size() == 1 && restored[0].total == 3 && restored[0].next_alarm_id == 4, "alarm sequence recovery");
    check(restored[0].open.size() == 2, "open alarm recovery");
    batt::Sessions recovered;
    recovered.restore_device("BT000", restored[0].open, restored[0].total, restored[0].next_alarm_id);
    const auto status = *recovered.snapshot_device("BT000", batt::Clock::now());
    check(status["state"] == "offline" && status["voltage_v"].is_null() && status["session_token"] == 0,
          "restart invented an online/fresh connection");
    check(status["alarm_history"] == 3 && status["open_alarms"] == 2, "recovered totals");
    std::atomic<std::int64_t> unused{1}; batt::Queries queries(unused);
    queries.persistence([&](const batt::QueryTask& t) { return store.create_query(t); },
                        [&](const batt::QueryTask& t) { store.finish_query(t); });
    const batt::Json fresh = {{"state","online_fresh"},{"session_token",1}};
    const auto now = batt::Clock::now();
    const auto first = queries.begin("BT000", fresh, now).task;
    check(first && first->id == 1 && store.task(1)->at("state") == "pending", "pending before send");
    const batt::Json response = {{"msgType",911},{"devId","BT000"},{"txnNo",1}};
    queries.response(1, response, true, now);
    auto task = *store.task(1);
    check(task["state"] == "success" && task["response"] == response, "complete response not persisted");
    check(!task["response"].contains("attrList"), "missing attrList silently turned into empty");
    check(task["connection_id"] == "boot-A:1" && !task["finished_ms"].is_null(), "task identity/time lost");
    const auto second = queries.begin("BT000", fresh, now).task;
    check(second && second->id == 2, "database sequence");
    store.initialize();
    check(store.task(2)->at("state") == "link_lost" && store.task(2)->at("reason") == "server_restart", "pending recovery");
    check(store.task(1)->at("state") == "success", "restart changed terminal state");
    auto offline = fresh; offline["state"] = "offline"; offline["session_token"] = 0;
    batt::Queries more(unused); more.persistence([&](const batt::QueryTask& t) { return store.create_query(t); }, {});
    check(more.begin("BT000", offline, now).task->id == 3, "restart reused query id");
    check(store.task(3)->at("connection_id").is_null(), "offline identity is not null");
    check(!store.task(9999), "unknown task exists");
}
void failures() {
    Temp tmp; batt::StoreOptions options; options.path = tmp.path;
    batt::StoreShared shared; std::atomic<bool> cancel{false};
    { batt::Store store(options, shared, cancel); store.initialize(); store.login("BT000", "SIM000", {}); }
    options.fail_operation = "alarm";
    batt::Store store(options, shared, cancel); batt::Sessions sessions;
    sessions.handle(1, hello(), batt::Clock::now());
    rejected([&] {
        sessions.persisted_handle(1, alarms({"03","06"}), batt::Clock::now(),
            [&](const batt::Sessions& candidate, const batt::SessionReply&) {
                store.alarms("BT000", {}, candidate.alarm_export("BT000"));
            });
    });
    check(store.alarm_rows("BT000").empty(), "failed alarm transaction partially committed");
    check(sessions.alarm_export("BT000").empty(), "failed alarm changed live cache");
    check(shared.failed, "storage did not enter failed state");
    std::atomic<std::int64_t> unused{1}; batt::Queries queries(unused);
    batt::Json fresh = {{"state","online_fresh"},{"session_token",1}};
    queries.persistence([](const batt::QueryTask&) -> std::int64_t { throw batt::StorageError("injected"); }, {});
    rejected([&] { queries.begin("BT000", fresh, batt::Clock::now()); });
    check(queries.stats()["tasks"] == 0 && queries.stats()["pending"] == 0, "failed allocation published task");
    queries.persistence([](const batt::QueryTask&) { return 71; },
                        [](const batt::QueryTask&) { throw batt::StorageError("injected"); });
    const auto now = batt::Clock::now(); queries.begin("BT000", fresh, now);
    const batt::Json response = {{"msgType",911},{"devId","BT000"},{"txnNo",71},{"attrList",batt::Json::array()}};
    rejected([&] { queries.response(1, response, true, now); });
    check(queries.find(71)->state == "pending" && !queries.find(71)->response, "failed terminal commit changed task cache");
    check(queries.stats()["pending"] == 1, "failed terminal commit removed pending entry");
    rejected([&] { queries.expire(now + std::chrono::seconds(4)); });
    check(queries.find(71)->state == "pending", "failed timeout commit changed task cache");
    rejected([&] { queries.disconnect(1, "peer_eof", now); });
    check(queries.find(71)->state == "pending", "failed disconnect commit changed task cache");
    rejected([&] {
        sessions.persisted_handle(2, hello(), now,
            [](const batt::Sessions&, const batt::SessionReply&) { throw batt::StorageError("injected"); });
    });
    check(sessions.device_for(1).has_value() && !sessions.device_for(2), "failed login replaced current session");
    rejected([] { batt::canonical_database(":memory:"); });
}
}
int main() {
    try { transactions(); failures(); std::cout << "store_test: " << checks << " checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << "store_test failed after " << checks << ": " << error.what() << '\n'; return 1; }
}
