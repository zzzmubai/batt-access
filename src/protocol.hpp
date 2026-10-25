/*
 * 文件作用：实现一台电池设备的内存业务状态，供 Sessions 调用。
 * 处理 110 登录、310 属性和 410 当前告警集合，生成 111/311/411 应答及日志事件。
 * 四项属性为电压、电流、SOC、温度，分别记录更新时间并判断数据是否齐全/过期。
 * 本层不操作 socket 和 SQLite；生产运行时由设备所属 worker 独占。
 * 911 查询响应交由 query_task.hpp 处理；持久化提交由 session.hpp 与 store.cpp 协调。
 * 配套阅读：tests/core_test.cpp 和 docs/fixtures/protocol-samples.json。
 */
#pragma once
#include "json_slice.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace batt {
using Json = nlohmann::json;
// 持久化时间使用 Unix 毫秒，便于重启后查看；运行中的过期判断使用单调时钟。
inline std::int64_t wall_millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// 用成熟 JSON 库校验完整对象，并额外拒绝重复键，避免同名字段被后值静默覆盖。
inline std::optional<Json> parse_object(const std::string& text) {
    bool duplicate = false;
    std::vector<std::set<std::string>> keys;
    const auto callback = [&](int, Json::parse_event_t event, Json& parsed) {
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::key) {
            if (keys.empty() || !keys.back().insert(parsed.get<std::string>()).second)
                duplicate = true;
        } else if (event == Json::parse_event_t::object_end && !keys.empty()) keys.pop_back();
        return true;
    };
    try {
        auto value = Json::parse(text, callback);
        if (duplicate || !value.is_object()) return std::nullopt;
        return value;
    } catch (const Json::exception&) { return std::nullopt; }
}

// 业务编号只接收可表示为有符号 64 位的整数，不把浮点数或过大无符号数强制转换。
inline std::optional<std::int64_t> integer(const Json& value) {
    if (!value.is_number_integer()) return std::nullopt;
    if (value.is_number_unsigned() &&
        value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return std::nullopt;
    return value.get<std::int64_t>();
}
inline bool identifier(const Json& value) {
    return value.is_string() && !value.get_ref<const std::string&>().empty() &&
           value.get_ref<const std::string&>().size() <= 64;
}

// ack 是给终端的报文，events 是本地日志；close_after_send 请求 I/O 在发送后关闭。
struct Reply {
    std::optional<Json> ack;
    std::vector<Json> events;
    bool close_after_send = false;
};
// 每个属性都有独立 updated；只收到 SOC 时，不能顺带刷新旧温度的年龄。
struct Reading {
    std::int64_t raw = 0;
    Time updated{};
    bool valid = false;
};
// id 是设备内部事件号，对应数据库 event_no；与 alarm 表的全局主键 id 不同。
struct Alarm {
    std::uint64_t id;
    std::string desc;
    Time first;
    Time last;
    std::optional<Time> cleared;
    std::int64_t first_ms = 0, last_ms = 0;
    std::optional<std::int64_t> clear_ms;
};

// Device business state, owned by its shard worker (unit tests call it directly).
class Protocol {
public:
    explicit Protocol(std::chrono::milliseconds freshness = std::chrono::seconds(90))
        : freshness_(freshness) {}
    bool logged_in() const { return !device_.empty(); }
    const std::string& device() const { return device_; }
    const std::vector<Alarm>& alarms() const { return history_; }
    std::size_t open_count() const { return open_.size(); }
    std::uint64_t alarm_count() const { return total_alarms_; }
    // 事务提交后只缓存 open 告警；完整历史保存在 SQLite，避免内存随运行时间持续增长。
    void discard_cleared() {
        std::vector<Alarm> open;
        for (auto& row : history_) if (!row.cleared) open.push_back(std::move(row));
        history_ = std::move(open); open_.clear();
        for (std::size_t i = 0; i < history_.size(); ++i) open_[history_[i].desc] = i;
    }
    // 恢复设备档案和 open 告警，但连接置为离线、属性清空；旧连接与单调时间不能跨进程恢复。
    void restore(const std::string& name, std::vector<Alarm> open,
                 std::uint64_t total, std::uint64_t next_id) {
        device_ = name; connected_ = false; readings_ = {}; history_ = std::move(open); open_.clear();
        total_alarms_ = total; next_alarm_id_ = next_id;
        for (std::size_t i = 0; i < history_.size(); ++i) open_[history_[i].desc] = i;
    }
    void restart_session() { readings_ = {}; connected_ = true; }
    void disconnect() { connected_ = false; }

    // 先看是否有连接，再看四项是否齐全，最后检查各项年龄；默认 freshness 为 90 秒。
    std::string status(Time now) const {
        if (!logged_in()) return "connected_unauthed";
        if (!connected_) return "offline";
        for (const auto& reading : readings_) if (!reading.valid) return "connected_no_data";
        for (const auto& reading : readings_)
            if (now - reading.updated > freshness_) return "connected_stale";
        return "online_fresh";
    }

    // 内部保留整数原始值；输出时电压、电流除以 100，SOC 和温度直接使用原值。
    Json snapshot(Time now) const {
        Json out = {{"devId", device_}, {"state", status(now)}};
        static const std::array<const char*, 4> names{{"voltage_v", "current_a", "soc", "temperature_c"}};
        for (std::size_t i = 0; i < readings_.size(); ++i) {
            if (!readings_[i].valid) out[names[i]] = nullptr;
            else if (i < 2) out[names[i]] = static_cast<double>(readings_[i].raw) / 100.0;
            else out[names[i]] = readings_[i].raw;
        }
        return out;
    }

    // 协议入口先校验公共信封，再按 msgType 分派；未登录或 devId 不匹配时不能修改设备数据。
    Reply handle(const Json& frame, Time now) {
        if (!frame.is_object() || !frame.contains("msgType") || !frame.contains("devId") ||
            !frame.contains("txnNo") || !identifier(frame["devId"])) return reject("invalid_envelope");
        const auto type = integer(frame["msgType"]);
        const auto txn = integer(frame["txnNo"]);
        if (!type || !txn || *txn <= 0) return reject("invalid_envelope");
        const auto dev = frame["devId"].get<std::string>();
        if (*type == 110) {
            const bool valid = frame.contains("imsi") && identifier(frame["imsi"]) &&
                frame.contains("devType") && integer(frame["devType"]) == std::optional<std::int64_t>(1) &&
                (!logged_in() || device_ == dev);
            if (!valid) {
                auto out = reject("invalid_login");
                out.ack = acknowledgement(111, dev, *txn, 0);
                out.close_after_send = true;
                return out;
            }
            const bool first = !logged_in();
            device_ = dev;
            connected_ = true;
            Reply out;
            out.ack = acknowledgement(111, dev, *txn);
            if (first) out.events.push_back({{"event", "login"}, {"devId", dev}});
            return out;
        }
        if (!logged_in() || dev != device_) return reject("unbound_device");
        if (*type == 310) return attributes(frame, *txn, now);
        if (*type == 410) return alarm_snapshot(frame, *txn, now);
        return reject("unsupported_type");
    }

private:
    static Json acknowledgement(int type, const std::string& dev, std::int64_t txn, int result = 1) {
        return {{"msgType", type}, {"devId", dev}, {"txnNo", txn}, {"result", result}};
    }
    static Reply reject(const std::string& reason) {
        Reply out;
        out.events.push_back({{"event", "rejected"}, {"reason", reason}});
        return out;
    }
    //收到 310 属性上报 → 校验并更新内存中的四项读数 → 回 311 应答（有实际更新时再记一条日志事件）。
    // 先在 next 中试算全部属性，整帧通过后才替换 readings_；防止非法 SOC 带来半帧更新。
    // 未知属性允许忽略；四个已用属性要求整数、范围合法且不能重复。
    Reply attributes(const Json& frame, std::int64_t txn, Time now) {
        if (!frame.contains("attrList") || !frame["attrList"].is_array() ||
            frame["attrList"].size() > 256) return reject("invalid_attributes");
        auto next = readings_;
        std::set<std::size_t> changed;
        static const std::array<std::string, 4> ids{{"01111001", "01112001", "01113001", "01119001"}};
        for (const auto& attr : frame["attrList"]) {
            if (!attr.is_object() || !attr.contains("id") || !attr["id"].is_string() ||
                !attr.contains("value")) return reject("invalid_attributes");
            const auto id = attr["id"].get<std::string>();
            for (std::size_t i = 0; i < ids.size(); ++i) {
                if (id != ids[i]) continue;
                const auto value = integer(attr["value"]);
                if (!value || *value < std::numeric_limits<std::int32_t>::min() ||
                    *value > std::numeric_limits<std::int32_t>::max() || !changed.insert(i).second ||
                    (i == 0 && *value < 0) || (i == 2 && (*value < 0 || *value > 100)))
                    return reject("invalid_attributes");
                next[i] = {*value, now, true};
            }
        }
        readings_ = next;
        Reply out;
        out.ack = acknowledgement(311, device_, txn);
        if (!changed.empty()) {
            auto event = snapshot(now);
            event["event"] = "attributes";
            out.events.push_back(std::move(event));
        }
        return out;
    }

    // 410 是当前集合快照：持续项更新 last，消失项恢复，新出现项创建事件。
    // 只处理 01003001 组的 01/03/06；000/0 表示该组清空，组缺席则保持原状态。
    Reply alarm_snapshot(const Json& frame, std::int64_t txn, Time now) {
        if (!frame.contains("alarmList") || !frame["alarmList"].is_array() ||
            frame["alarmList"].size() > 256) return reject("invalid_alarms");
        bool has_group = false;
        bool normal = false;
        bool any_active = false;
        std::size_t group_size = 0;
        std::set<std::string> seen;
        std::set<std::string> active;
        for (const auto& item : frame["alarmList"]) {
            if (!item.is_object() || !item.contains("id") || !item["id"].is_string())
                return reject("invalid_alarms");
            if (item["id"] != "01003001") continue;
            has_group = true;
            if (++group_size > 64 || !item.contains("alarmDesc") || !item["alarmDesc"].is_string() ||
                !item.contains("alarmFlag")) return reject("invalid_alarms");
            const auto desc = item["alarmDesc"].get<std::string>();
            const auto flag = integer(item["alarmFlag"]);
            if (!seen.insert(desc).second || !flag || (*flag != 0 && *flag != 1))
                return reject("invalid_alarms");
            if (desc == "000") {
                if (*flag != 0) return reject("invalid_alarms");
                normal = true;
            } else {
                any_active = any_active || *flag == 1;
                if (desc == "01" || desc == "03" || desc == "06") {
                    if (*flag != 1) return reject("invalid_alarms");
                    active.insert(desc);
                }
            }
        }
        // 同一帧既表示全部恢复又包含活跃告警是矛盾输入，整帧拒绝而非部分应用。
        if (normal && any_active) return reject("conflicting_alarm_snapshot");
        Reply out;
        out.ack = acknowledgement(411, device_, txn);
        if (!has_group) return out;
        std::size_t new_events = 0;
        for (const auto& desc : active) if (open_.count(desc) == 0) ++new_events;
        if (history_.size() + new_events > 4096) {
            auto rejected = reject("memory_alarm_history_limit");
            rejected.close_after_send = true;
            return rejected;
        }
        // 完整集合中不再出现的已知项转为 cleared；离线本身不会自动清除告警。
        for (auto it = open_.begin(); it != open_.end();) {
            if (active.count(it->first) == 0) {
                history_[it->second].cleared = now;
                history_[it->second].clear_ms = wall_millis();
                it = open_.erase(it);
            } else ++it;
        }
        for (const auto& desc : active) {
            const auto found = open_.find(desc);
            if (found == open_.end()) {
                const auto index = history_.size();
                const auto wall = wall_millis();
                history_.push_back({next_alarm_id_++, desc, now, now, std::nullopt, wall, wall, std::nullopt});
                ++total_alarms_;
                open_.emplace(desc, index);
            } else {
                history_[found->second].last = now;
                history_[found->second].last_ms = wall_millis();
            }
        }
        out.events.push_back({{"event", "alarms"}, {"devId", device_},
                              {"open", open_.size()}, {"history", total_alarms_}});
        return out;
    }

    std::string device_;
    std::chrono::milliseconds freshness_;
    bool connected_ = false;
    std::array<Reading, 4> readings_{};
    std::map<std::string, std::size_t> open_;
    std::vector<Alarm> history_;
    std::uint64_t total_alarms_ = 0, next_alarm_id_ = 1;
};
} // namespace batt
