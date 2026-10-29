#include "api.hpp"
#include "store.hpp"
#include "mysql_runtime.hpp"
#include <cstdlib>
#include <iostream>
#include <thread>
#include <unistd.h>

namespace {
unsigned checks=0;
void check(bool value,const char* description) { ++checks; if (!value) throw std::runtime_error(description); }
void contexts() {
    batt::ApiContext late(1,batt::Clock::now()-std::chrono::milliseconds(1),true,"retry-me");
    const auto expired=late.wait();
    check(expired.status==503 && expired.body["outcome"]=="unknown","ambiguous timeout");
    late.complete({201,{{"txnNo",99}},""});
    check(late.wait().status==503,"late completion overwrote timed-out response");
    batt::ApiContext ready(2,batt::Clock::now()+std::chrono::seconds(1),false,"");
    ready.complete({200,{{"devId","BT001"}},""}); ready.complete({404,batt::Json::object(),""});
    check(ready.wait().status==200 && ready.wait().body["requestId"]==2,"completion is not one-shot");
    batt::ApiBridge bridge(std::chrono::seconds(2),1);
    batt::ApiReply first;
    std::thread caller([&] { first=bridge.call(batt::Command{}); });
    const auto until=batt::Clock::now()+std::chrono::seconds(1);
    while (bridge.stats()["admitted"]==0 && batt::Clock::now()<until) std::this_thread::yield();
    const auto second=bridge.call(batt::Command{});
    auto command=bridge.take();
    if (command) command->context->complete({200,{{"value",7}},""});
    caller.join();
    check(command.has_value() && first.status==200 && first.body["value"]==7,"bridge result association");
    check(second.status==503 && second.body["error"]["code"]=="api_busy","bridge overflow not rejected");
    check(first.body["requestId"]!=second.body["requestId"],"request identity reused");
    batt::ApiReply stopped;
    std::thread waiting([&] { stopped=bridge.call(batt::Command{}); });
    const auto stop_until=batt::Clock::now()+std::chrono::seconds(1);
    while (bridge.stats()["admitted"]!=2 && batt::Clock::now()<stop_until) std::this_thread::yield();
    bridge.close(); waiting.join();
    check(stopped.status==503 && stopped.body["error"]["code"]=="server_stopping","close did not wake waiter");
    check(bridge.call(batt::Command{}).status==503,"closed bridge admitted work");
    check(bridge.stats()["queue_peak"]==1,"bridge capacity exceeded");
}
struct Temp {
    std::string path;
    Temp() {
        static unsigned next = 0;
        const auto name = std::string("BATT_TEST_DB_CONFIG_") + std::to_string(next++);
        const auto* value = std::getenv(name.c_str());
        if (!value) throw std::runtime_error("isolated MySQL test configuration missing");
        path = value;
    }
};
void admission() {
    Temp temp; batt::StoreOptions options; options.db=batt::load_db_config(temp.path); options.boot_id="unit";
    batt::StoreShared shared; std::atomic<bool> cancelled{false};
    batt::Store store(options,shared,cancelled); store.initialize(); store.login("BT000","SIM",{}); store.login("BT001","SIM",{});
    std::atomic<std::int64_t> unused{1}; batt::Queries queries(unused);
    const batt::Json fresh={{"state","online_fresh"},{"session_token",1}};
    auto draft=queries.prepare("BT000",fresh,batt::Clock::now());
    auto first=store.create_api_query("same-key","BT000",draft);
    check(first.created && first.created->id==1,"API creation failed");
    queries.publish(*first.created);
    const auto busy=queries.prepare("BT000",fresh,batt::Clock::now());
    check(!busy.task && busy.rejected=="busy","pending check");
    const auto replay=store.create_api_query("same-key","BT000",busy);
    check(replay.replayed && replay.replayed->at("txnNo")==1 && !replay.created,"dedup must precede busy");
    auto other=queries.prepare("BT001",fresh,batt::Clock::now());
    const auto conflict=store.create_api_query("same-key","BT001",other);
    check(conflict.rejected=="request_id_conflict" && !shared.failed,"normal conflict poisoned storage");
    const auto rejected=store.create_api_query("new-key","BT000",busy);
    check(rejected.rejected=="busy" && !rejected.created,"new request bypassed pending guard");
    queries.disconnect(1,"peer_eof",batt::Clock::now()); // No callback here: finish durable task explicitly below.
    auto finished=*first.created; finished.state="link_lost"; finished.reason="peer_eof"; finished.finished_ms=batt::wall_millis();
    store.finish_query(finished);
    check(store.create_api_query("same-key","BT000",busy).replayed->at("state")=="link_lost","terminal replay changed state");
    batt::ApiAdmission results[2]; std::exception_ptr errors[2];
    std::thread threads[2];
    for (unsigned i=0;i<2;++i) threads[i]=std::thread([&,i] {
        try {
            batt::Store connection(options,shared,cancelled);
            auto candidate=*draft.task; candidate.device=i==0?"BT000":"BT001";
            results[i]=connection.create_api_query("race-key",candidate.device,{candidate,""});
        } catch (...) { errors[i]=std::current_exception(); }
    });
    for (auto& thread : threads) thread.join();
    check(!errors[0] && !errors[1],"cross-shard dedup threw exception");
    check(static_cast<unsigned>(results[0].created.has_value())+static_cast<unsigned>(results[1].created.has_value())==1,"cross-shard duplicate task");
    check((results[0].rejected=="request_id_conflict") != (results[1].rejected=="request_id_conflict"),"cross-shard conflict result");
    check(!shared.failed,"cross-shard conflict failed service");
    auto page=store.alarm_page("BT000","",20,std::numeric_limits<std::int64_t>::max());
    check(page["items"].empty() && page["nextBeforeId"].is_null(),"empty pagination");
}
}
int main() {
    try { batt::MysqlLibrary mysql_library; contexts(); admission(); std::cout<<"api_test: "<<checks<<" checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr<<"api_test failed after "<<checks<<": "<<error.what()<<'\n'; return 1; }
}
