#include "worker.hpp"
#include "send_buffer.hpp"
#include <iostream>

namespace {
unsigned checks = 0;
void check(bool value, const char* message) {
    ++checks; if (!value) throw std::runtime_error(message);
}
batt::Time at(int ms) { return batt::Time{} + std::chrono::milliseconds(ms); }
batt::Json snapshot(const std::string& state = "online_fresh", batt::Token token = 1) {
    return {{"devId", "BT000"}, {"state", state}, {"session_token", token}};
}
batt::Json response(std::int64_t id, const std::string& dev = "BT000") {
    return {{"msgType", 911}, {"devId", dev}, {"txnNo", id},
            {"attrList", {{{"id", "8001"}, {"protect", 4150}}}}};
}
void queries() {
    std::atomic<std::int64_t> next{1}; batt::Queries q(next);
    check(q.begin("MISSING", std::nullopt, at(0)).rejected == "not_found", "unknown query accepted");
    auto started = q.begin("BT000", snapshot(), at(0));
    check(started.task && started.task->id == 1 && started.task->state == "pending", "query allocation");
    check(q.begin("BT000", snapshot(), at(1)).rejected == "busy", "duplicate pending query accepted");
    check(q.expire(at(2999)).empty(), "premature timeout");
    q.response(1, response(1), true, at(2999));
    check(q.find(1)->state == "success" && q.find(1)->response.has_value(), "valid response not stored");
    q.response(1, response(1), true, at(4000));
    check(q.find(1)->state == "success" && q.stats()["duplicate_response"] == 1, "duplicate reversed success");
    auto second = q.begin("BT000", snapshot(), at(5000));
    check(second.task && second.task->id == 2, "query id not monotonic");
    q.response(1, response(2), true, at(8000));
    check(q.find(2)->state == "timeout", "deadline equality accepted as success");
    q.response(1, response(2), true, at(8100));
    check(q.find(2)->state == "timeout" && q.stats()["late_frames"] == 2 && q.stats()["late_tasks"] == 1,
          "late response counters/state");
    auto third = q.begin("BT000", snapshot(), at(9000));
    check(third.task.has_value(), "new query after timeout rejected");
    q.disconnect(1, "replaced", at(9010));
    check(q.find(third.task->id)->state == "link_lost" && q.find(third.task->id)->reason == "replaced", "replacement not terminal");
    q.response(2, response(third.task->id), true, at(9020));
    check(q.find(third.task->id)->state == "link_lost" && q.stats()["orphan_response"] == 1, "new session completed old query");
    auto fourth = q.begin("BT000", snapshot(), at(10000));
    auto invalid = response(fourth.task->id); invalid["attrList"] = {1};
    q.response(1, invalid, true, at(10001));
    check(q.find(fourth.task->id)->state == "pending", "malformed result accepted");
    auto empty = response(fourth.task->id); empty.erase("attrList");
    q.response(1, empty, true, at(10002));
    check(q.find(fourth.task->id)->state == "success" && !q.find(fourth.task->id)->response->contains("attrList"),
          "missing attrList not preserved");
    auto offline = q.begin("BT000", snapshot("offline", 0), at(11000));
    check(offline.task && offline.task->state == "link_lost" && offline.task->reason == "offline", "offline not recorded");
    auto pending = q.begin("BT000", snapshot(), at(12000));
    q.shutdown(at(12001));
    check(q.find(pending.task->id)->state == "link_lost" && q.find(pending.task->id)->reason == "server_shutdown", "shutdown left pending");
    batt::Queries other(next);
    check(other.begin("BT001", snapshot(), at(0)).task->id > pending.task->id, "workers reused query ID");
    batt::Queries limited(next, std::chrono::seconds(3), 1);
    limited.begin("BT000", snapshot(), at(0));
    check(limited.begin("BT001", snapshot(), at(0)).rejected == "memory_task_limit", "task storage is unbounded");
    batt::Queries timer(next);
    const auto last = timer.begin("BT000", snapshot(), at(0)).task->id;
    check(timer.expire(at(3000)).size() == 1 && timer.find(last)->state == "timeout", "idle timeout failed");
}
void queues() {
    batt::BoundedQueue<int> q(1);
    check(q.try_push(10) && !q.try_push(20), "queue limit");
    check(q.size() == 1 && q.peak() == 1, "queue statistics");
    check(q.try_pop() == std::optional<int>(10) && !q.try_pop(), "queue FIFO/pop");
    q.try_push(30);
    std::atomic<bool> abort{false}, started{false}, result{true};
    std::thread producer([&] { started.store(true); result.store(q.push_wait(40, abort)); });
    while (!started.load()) std::this_thread::yield();
    q.close(); producer.join();
    check(!result.load(), "closed queue did not wake producer");
    check(q.try_pop() == std::optional<int>(30) && q.drained(), "close did not drain admitted work");
    check(!q.try_push(50), "closed queue accepted new work");
    batt::BoundedQueue<int> waiting(1); std::atomic<bool> popped{true};
    std::thread consumer([&] { popped.store(waiting.pop_for(std::chrono::seconds(5)).has_value()); });
    waiting.close(); consumer.join(); check(!popped.load(), "closed queue did not wake consumer");
    check(batt::device_shard("BT000", 2) == batt::device_shard("BT002", 2), "stable shard mapping");
    check(batt::device_shard("BT000", 2) != batt::device_shard("BT001", 2), "test devices not on separate shards");
}
void send_deadlines() {
    batt::SendBuffer pending;
    pending.enqueue("{}", at(10)); pending.enqueue("[]");
    std::string output;
    auto writer = [&](const char* bytes, std::size_t count) -> std::ptrdiff_t {
        output.append(bytes, count); return static_cast<std::ptrdiff_t>(count);
    };
    check(pending.flush(writer, 10, 100, [] { return at(10); }) == batt::Flush::drained && output == "[]",
          "expired unsent query was sent");
    batt::SendBuffer partial; output.clear(); partial.enqueue("{}", at(10));
    check(partial.flush(writer, 1, 1, [] { return at(0); }) == batt::Flush::pending && output == "{", "partial send setup");
    check(partial.flush(writer, 1, 100, [] { return at(10); }) == batt::Flush::deadline_partial && output == "{",
          "expired partial query did not require closing stream");
}
} // namespace
int main() {
    try { queries(); queues(); send_deadlines(); std::cout << "query_test: " << checks << " checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << "query_test: " << error.what() << '\n'; return 1; }
}
