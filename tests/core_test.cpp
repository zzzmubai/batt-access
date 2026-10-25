#include "protocol.hpp"
#include "send_buffer.hpp"
#include "session.hpp"
#include <iostream>
#include <random>

namespace {
unsigned checks = 0;
void check(bool value, const std::string& what) {
    ++checks;
    if (!value) throw std::runtime_error(what);
}
template<class F> void throws(F action, const std::string& what) {
    bool caught = false;
    try { action(); } catch (const std::exception&) { caught = true; }
    check(caught, what);
}
const std::string login_wire = R"({"msgType":110,"devId":"BT000","imsi":"SIM000","devType":1,"txnNo":1})";
const std::string attr_wire = R"({"msgType":310,"devId":"BT000","txnNo":2,"attrList":[{"id":"01111001","value":5280},{"id":"01112001","value":-125},{"id":"01113001","value":80},{"id":"01119001","value":28}]})";
batt::Json login() { return batt::Json::parse(login_wire); }
batt::Json attrs() { return batt::Json::parse(attr_wire); }
batt::Time time_at(int ms) { return batt::Time{} + std::chrono::milliseconds(ms); }
batt::Json alarm(std::int64_t txn, std::vector<std::string> active) {
    batt::Json items = batt::Json::array();
    for (const auto& desc : active)
        items.push_back({{"id", "01003001"}, {"alarmDesc", desc}, {"alarmFlag", 1}});
    if (active.empty()) items.push_back({{"id", "01003001"}, {"alarmDesc", "000"}, {"alarmFlag", 0}});
    return {{"msgType", 410}, {"devId", "BT000"}, {"txnNo", txn}, {"alarmList", items}};
}

void slices() {
    for (std::size_t cut = 1; cut < login_wire.size(); ++cut) {
        batt::JsonSlice parser;
        std::vector<std::string> found;
        auto emit = [&](std::string&& x) { found.push_back(std::move(x)); };
        parser.feed(std::string_view(login_wire).substr(0, cut), time_at(0), emit);
        check(found.empty(), "partial login emitted too early");
        parser.feed(std::string_view(login_wire).substr(cut), time_at(1), emit);
        check(found == std::vector<std::string>{login_wire}, "single split changed frame");
    }
    std::mt19937 random(20261019);
    const std::string text = " \n" + login_wire + attr_wire + R"({"s":"braces {}[] quote \" slash \\"})";
    for (int pass = 0; pass < 100; ++pass) {
        batt::JsonSlice parser;
        std::vector<std::string> found;
        for (std::size_t pos = 0; pos < text.size();) {
            const auto count = std::min(text.size() - pos, static_cast<std::size_t>(1 + random() % 13));
            parser.feed(std::string_view(text).substr(pos, count), time_at(0),
                [&](std::string&& x) { found.push_back(std::move(x)); });
            pos += count;
        }
        check(found.size() == 3 && found[0] == login_wire && found[1] == attr_wire,
              "random fragmentation/escaping corrupted boundaries");
        check(batt::parse_object(found[2]).has_value(), "escaped JSON must be valid");
    }
    batt::JsonSlice one_byte;
    unsigned count = 0;
    for (const char ch : login_wire) one_byte.feed(std::string_view(&ch, 1), time_at(0),
        [&](std::string&&) { ++count; });
    check(count == 1, "one byte slices duplicated login");
    batt::JsonSlice malformed;
    std::vector<std::string> found;
    malformed.feed("{\"x\":1,}" + login_wire, time_at(0),
        [&](std::string&& x) { found.push_back(std::move(x)); });
    check(found.size() == 2 && !batt::parse_object(found[0]) && batt::parse_object(found[1]),
          "balanced malformed JSON must not consume next object");
    batt::JsonSlice partial;
    partial.feed("{\"x\":\"unfinished", time_at(10), [](std::string&&) {});
    check(!partial.expired(time_at(5009), std::chrono::seconds(5)), "premature frame timeout");
    check(partial.expired(time_at(5010), std::chrono::seconds(5)), "frame timeout boundary");
    throws([] { batt::JsonSlice p(8); p.feed("{\"x\":123}", time_at(0), [](std::string&&) {}); },
           "closed oversize object accepted");
    throws([] { batt::JsonSlice p(100, 2); p.feed("{\"a\":[{}]}", time_at(0), [](std::string&&) {}); },
           "depth limit ignored");
    throws([] { batt::JsonSlice p; p.feed("garbage{}", time_at(0), [](std::string&&) {}); },
           "invalid prefix silently skipped");
    check(!batt::parse_object(R"({"devId":"A","devId":"B"})"), "duplicate root key accepted");
    check(!batt::parse_object(R"({"x":[{"a":1,"a":2}]})"), "duplicate nested key accepted");
    check(!batt::integer(batt::Json(true)), "bool accepted as integer");
    check(!batt::integer(batt::Json::parse("18446744073709551615")), "uint64 overflow accepted");
}

void protocol() {
    batt::Protocol p;
    check(!p.handle(attrs(), time_at(0)).ack, "unauthed attributes accepted");
    const auto hello = p.handle(login(), time_at(0));
    check(hello.ack && (*hello.ack)["msgType"] == 111 && (*hello.ack)["txnNo"] == 1,
          "login acknowledgement envelope");
    check(hello.events.size() == 1 && p.handle(login(), time_at(1)).events.empty(), "login not idempotent");
    auto good = attrs();
    good["attrList"].push_back({{"id", "01102001"}, {"value", 120.12}});
    good["metadata"] = {{"enabled", true}, {"optional", nullptr}};
    check(p.handle(good, time_at(10)).ack.has_value(), "unknown legal JSON rejected");
    const auto snapshot = p.snapshot(time_at(10));
    check(snapshot["voltage_v"] == 52.8 && snapshot["current_a"] == -1.25 &&
          snapshot["soc"] == 80 && snapshot["temperature_c"] == 28, "unit conversion");
    auto invalid = attrs();
    invalid["attrList"][0]["value"] = 6000;
    invalid["attrList"][2]["value"] = 101;
    check(!p.handle(invalid, time_at(20)).ack, "illegal SOC accepted");
    check(p.snapshot(time_at(20))["voltage_v"] == 52.8, "partial update on rejected frame");
    auto duplicate = attrs();
    duplicate["attrList"].push_back(duplicate["attrList"][0]);
    check(!p.handle(duplicate, time_at(30)).ack, "duplicate property id accepted");
    auto impostor = attrs();
    impostor["devId"] = "OTHER";
    check(!p.handle(impostor, time_at(30)).ack, "connection device binding ignored");
    auto partial = attrs();
    partial["attrList"] = batt::Json::array({{{"id", "01113001"}, {"value", 81}}});
    p.handle(partial, time_at(90011));
    check(p.status(time_at(90011)) == "connected_stale", "SOC refreshed stale temperature");
    batt::Protocol q;
    q.handle(login(), time_at(0));
    q.handle(partial, time_at(1));
    check(q.status(time_at(1)) == "connected_no_data", "partial snapshot became fresh");
    auto failed_login = login();
    failed_login["devType"] = 2;
    check(q.handle(failed_login, time_at(2)).close_after_send, "invalid login not rejected");
}

void alarms() {
    batt::Protocol p;
    p.handle(login(), time_at(0));
    p.handle(alarm(2, {"03"}), time_at(10));
    p.handle(alarm(3, {"03"}), time_at(20));
    check(p.alarms().size() == 1 && p.open_count() == 1 && p.alarms()[0].last == time_at(20),
          "repeated alarm created duplicate history");
    p.handle(alarm(4, {"01", "03"}), time_at(30));
    p.handle(alarm(5, {"01"}), time_at(40));
    check(p.open_count() == 1 && p.alarms()[0].cleared == time_at(40), "subset recovery");
    auto absent = alarm(6, {});
    absent["alarmList"] = batt::Json::array();
    p.handle(absent, time_at(50));
    check(p.open_count() == 1, "absent alarm group cleared state");
    auto conflict = alarm(7, {"03"});
    conflict["alarmList"].push_back({{"id", "01003001"}, {"alarmDesc", "000"}, {"alarmFlag", 0}});
    check(!p.handle(conflict, time_at(60)).ack && p.open_count() == 1, "conflicting alarm partially applied");
    p.handle(alarm(8, {}), time_at(70));
    check(p.open_count() == 0 && p.alarms().size() == 2, "000 did not clear all");
    p.handle(alarm(9, {"03"}), time_at(80));
    check(p.open_count() == 1 && p.alarms().size() == 3 && p.alarms().back().id == 3,
          "retrigger reused previous event");
}

void sending() {
    batt::SendBuffer pending(20);
    pending.enqueue("abcdef");
    pending.enqueue("XYZ");
    std::string output;
    int calls = 0;
    const auto writer = [&](const char* bytes, std::size_t n) -> std::ptrdiff_t {
        ++calls;
        if (calls == 1) { errno = EINTR; return -1; }
        if (calls == 3) { errno = EAGAIN; return -1; }
        const auto count = std::min<std::size_t>(n, 2);
        output.append(bytes, count);
        return static_cast<std::ptrdiff_t>(count);
    };
    check(pending.flush(writer) == batt::Flush::pending && pending.size() == 7,
          "EAGAIN lost pending bytes");
    check(pending.flush(writer) == batt::Flush::drained && output == "abcdefXYZ" && pending.size() == 0,
          "short write offset corrupted output");
    pending.enqueue("12345678901234567890");
    throws([&] { pending.enqueue("x"); }, "send cap ignored");
    check(pending.size() == 20, "overflow changed send buffer");
    check(pending.flush([](const char*, std::size_t) -> std::ptrdiff_t { errno = EPIPE; return -1; }) ==
          batt::Flush::disconnected, "EPIPE not reported");
    batt::SendBuffer limited;
    limited.enqueue("012345");
    output.clear();
    auto capture = [&](const char* bytes, std::size_t n) -> std::ptrdiff_t {
        check(n <= 1, "test chunk limit ignored");
        output.append(bytes, n);
        return static_cast<std::ptrdiff_t>(n);
    };
    check(limited.flush(capture, 1, 3) == batt::Flush::pending && output == "012", "send budget ignored");
    check(limited.flush(capture, 1) == batt::Flush::drained && output == "012345", "budget resume failed");
}
void sessions() {
    batt::Sessions store;
    check(store.handle(1, login(), time_at(0)).reply.ack.has_value(), "session login failed");
    store.handle(1, attrs(), time_at(1));
    store.handle(1, alarm(3, {"03"}), time_at(2));
    auto invalid = login();
    invalid["devType"] = 2;
    check((*store.handle(2, invalid, time_at(3)).reply.ack)["result"] == 0,
          "invalid replacement accepted");
    check(store.device_for(1) == std::optional<std::string>("BT000") && !store.device_for(2),
          "invalid login replaced current connection");
    check(store.handle(1, login(), time_at(4)).reply.events.empty(), "same connection login duplicated");
    check((*store.snapshot_for(1, time_at(4)))["state"] == "online_fresh",
          "duplicate login reset fresh attributes");
    auto next = store.handle(3, login(), time_at(5));
    check(next.replaced == std::optional<batt::Token>(1), "old token not returned");
    check(!store.device_for(1) && store.device_for(3).has_value(), "token replacement not atomic");
    check((*store.snapshot_for(3, time_at(6)))["state"] == "connected_no_data" &&
          (*store.snapshot_for(3, time_at(6)))["voltage_v"].is_null(), "new connection inherited freshness");
    check((*store.snapshot_for(3, time_at(6)))["open_alarms"] == 1, "replacement lost open alarm");
    auto stale = attrs(); stale["attrList"][0]["value"] = 9999;
    check(!store.handle(1, stale, time_at(7)).reply.ack, "old token updated device");
    check(!store.disconnect(1), "old disconnect detached new connection");
    check(store.device_for(3).has_value(), "new connection lost after stale disconnect");
    store.handle(3, attrs(), time_at(8));
    check((*store.snapshot_for(3, time_at(8)))["voltage_v"] == 52.8, "stale frame contaminated voltage");
    auto other = login(); other["devId"] = "BT001";
    store.handle(4, other, time_at(9));
    auto other_attrs = attrs(); other_attrs["devId"] = "BT001"; other_attrs["attrList"][0]["value"] = 6100;
    store.handle(4, other_attrs, time_at(10));
    check((*store.snapshot_for(4, time_at(10)))["voltage_v"] == 61.0 &&
          (*store.snapshot_for(3, time_at(10)))["voltage_v"] == 52.8, "devices shared attributes");
    auto forged = attrs(); forged["devId"] = "BT001";
    check(!store.handle(3, forged, time_at(11)).reply.ack, "device spoof accepted");
    check(store.disconnect(3), "current disconnect ignored");
    check((*store.snapshot_device("BT000", time_at(12)))["state"] == "offline", "offline missing");
    check((*store.snapshot_device("BT001", time_at(12)))["state"] == "online_fresh", "other device detached");
    store.handle(5, login(), time_at(13));
    check((*store.snapshot_for(5, time_at(13)))["alarm_history"] == 1, "reconnect lost alarm history");
    store.handle(5, alarm(4, {"03"}), time_at(14));
    check((*store.snapshot_for(5, time_at(14)))["alarm_history"] == 1, "reconnect duplicated open alarm");
    check(!store.disconnect(3) && store.device_for(5).has_value(), "late old close won over reconnect");
    auto other_alarm = alarm(5, {"03"}); other_alarm["devId"] = "BT001";
    store.handle(4, other_alarm, time_at(15));
    store.handle(5, alarm(6, {}), time_at(16));
    check((*store.snapshot_for(5, time_at(16)))["open_alarms"] == 0, "target alarm did not clear");
    check((*store.snapshot_for(4, time_at(16)))["open_alarms"] == 1, "alarm clear leaked to another device");
    batt::Sessions limited(1);
    limited.handle(10, login(), time_at(0));
    const auto full = limited.handle(11, other, time_at(0));
    check(full.reply.ack && (*full.reply.ack)["result"] == 0 && limited.registered() == 1,
          "device record capacity ignored");
}
} // namespace

int main() {
    try {
        slices(); protocol(); alarms(); sending(); sessions();
        std::cout << "core_test: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "core_test failed after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
