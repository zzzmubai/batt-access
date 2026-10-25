/*
 * 文件作用：管理设备档案与当前连接之间的绑定，供每个 worker 独占使用。
 * 一个 devId 对应一个 Device/Protocol，当前会话由独立 Token 标识。
 * 合法新连接可以接管同名设备；旧连接的报文或关闭通知不得影响接管后的会话。
 * persisted_handle 为登录和告警创建候选状态，数据库提交成功后才发布到正式缓存。
 * 阅读顺序：先理解 protocol.hpp 的单设备状态，再看本文件的多设备与连接关系。
 */
#pragma once
#include "protocol.hpp"
#include <functional>

namespace batt {
// Tokens identify accepts, never operating-system file descriptor numbers.
// 每次 accept 分配新 token；fd 会被操作系统复用，因此不能充当业务连接身份。
using Token = std::uint64_t;
struct SessionReply {
    Reply reply;
    std::optional<Token> replaced;
};

class Sessions {
public:
    explicit Sessions(std::size_t limit = 1024,
                      std::chrono::milliseconds freshness = std::chrono::seconds(90))
        : limit_(limit), freshness_(freshness) {}

    // 登录走 attach；其他上报只能交给该 token 当前绑定的设备。
    SessionReply handle(Token token, const Json& frame, Time now) {
        const bool login = frame.is_object() && frame.contains("msgType") &&
            integer(frame["msgType"]) == std::optional<std::int64_t>(110);
        if (login) return attach(token, frame, now);
        auto* device = current(token);
        if (!device) {
            Protocol unbound;
            return {unbound.handle(frame, now), std::nullopt};
        }
        return {device->protocol.handle(frame, now), std::nullopt};
    }
    // 只复制目标设备构建候选状态，不拷贝整个分片；commit 抛异常时正式缓存保持原样。
    SessionReply persisted_handle(Token token, const Json& frame, Time now,
        const std::function<void(const Sessions&, const SessionReply&)>& commit) {
        const auto bound = device_for(token);
        const auto name = bound ? *bound :
            (frame.contains("devId") && identifier(frame["devId"]) ? frame["devId"].get<std::string>() : std::string());
        if (name.empty()) return handle(token, frame, now);
        const auto before = devices_.find(name);
        Sessions candidate(before == devices_.end() && devices_.size() >= limit_ ? 0 : limit_, freshness_);
        if (before != devices_.end()) {
            candidate.devices_.emplace(name, before->second);
            if (before->second.current) candidate.bindings_[before->second.current] = name;
        }
        auto result = candidate.handle(token, frame, now);
        if (!result.reply.ack || result.reply.ack->value("result", 0) != 1) return result;
        // 先提交，再替换缓存和允许成功应答；提交后 ACK 丢失仍可能发生，重发由集合语义收敛。
        commit(candidate, result); // No published state or success acknowledgement before COMMIT.
        candidate.devices_.at(name).protocol.discard_cleared(); // History is authoritative in SQLite.
        if (before != devices_.end() && before->second.current) bindings_.erase(before->second.current);
        devices_.insert_or_assign(name, std::move(candidate.devices_.at(name)));
        if (devices_.at(name).current) bindings_[devices_.at(name).current] = name;
        return result;
    }
    void restore_device(const std::string& dev, std::vector<Alarm> open,
                        std::uint64_t total, std::uint64_t next_id) {
        if (devices_.size() >= limit_) throw std::runtime_error("device cache capacity exceeded during recovery");
        auto it = devices_.try_emplace(dev, freshness_).first;
        it->second.protocol.restore(dev, std::move(open), total, next_id);
    }
    std::vector<Alarm> alarm_export(const std::string& dev) const {
        const auto found = devices_.find(dev);
        return found == devices_.end() ? std::vector<Alarm>() : found->second.protocol.alarms();
    }

    // 双向核对 token 绑定和设备的 current，替换后的旧 token 在这里会失效。
    std::optional<std::string> device_for(Token token) const {
        const auto bound = bindings_.find(token);
        if (bound == bindings_.end()) return std::nullopt;
        const auto device = devices_.find(bound->second);
        if (device == devices_.end() || device->second.current != token) return std::nullopt;
        return bound->second;
    }

    std::optional<Json> snapshot_for(Token token, Time now) const {
        const auto dev = device_for(token);
        return dev ? snapshot_device(*dev, now) : std::nullopt;
    }

    std::optional<Json> snapshot_device(const std::string& dev, Time now) const {
        const auto found = devices_.find(dev);
        if (found == devices_.end()) return std::nullopt;
        auto out = found->second.protocol.snapshot(now);
        out["open_alarms"] = found->second.protocol.open_count();
        out["alarm_history"] = found->second.protocol.alarm_count();
        out["session_token"] = found->second.current;
        return out;
    }

    // A delayed close belonging to a replaced token cannot detach its successor.
    // 只结束仍然匹配的当前会话，保留设备档案和 open 告警。
    bool disconnect(Token token) {
        const auto name = device_for(token);
        if (!name) return false;
        auto& device = devices_.at(*name);
        device.current = 0;
        device.protocol.disconnect();
        bindings_.erase(token);
        return true;
    }

    // worker 定期调用；没有新报文时也能发现属性由 fresh 变为 stale，并仅在状态变化时产生日志。
    std::vector<Json> state_changes(Time now) {
        std::vector<Json> changed;
        for (auto& item : devices_) {
            auto& device = item.second;
            const auto state = device.protocol.status(now);
            if (state != device.reported_state) {
                device.reported_state = state;
                auto event = *snapshot_device(item.first, now);
                event["event"] = "device_state";
                changed.push_back(std::move(event));
            }
        }
        return changed;
    }
    std::size_t registered() const { return devices_.size(); }
    std::vector<Json> snapshots(Time now) const {
        std::vector<Json> result;
        for (const auto& item : devices_) result.push_back(*snapshot_device(item.first, now));
        return result;
    }
    std::vector<Json> alarm_rows(const std::string& dev) const {
        std::vector<Json> result;
        const auto found = devices_.find(dev);
        if (found == devices_.end()) return result;
        for (const auto& alarm : found->second.protocol.alarms())
            result.push_back({{"event", "alarm"}, {"devId", dev}, {"id", alarm.id},
                              {"desc", alarm.desc}, {"state", alarm.cleared ? "cleared" : "open"}});
        return result;
    }

private:
    struct Device {
        explicit Device(std::chrono::milliseconds freshness) : protocol(freshness) {}
        Protocol protocol;
        Token current = 0;
        std::string reported_state;
    };

    Device* current(Token token) {
        const auto name = device_for(token);
        return name ? &devices_.at(*name) : nullptr;
    }

    // 先完整校验新登录，验证失败不能踢掉合法旧连接；同连接重复登录也不会清空属性。
    SessionReply attach(Token token, const Json& frame, Time now) {
        // Validate before changing the current connection or touching old data.
        Protocol candidate;
        auto validation = candidate.handle(frame, now);
        if (!validation.ack || (*validation.ack)["result"] != 1)
            return {std::move(validation), std::nullopt};
        const auto name = frame["devId"].get<std::string>();
        const auto existing = device_for(token);
        if (existing && *existing != name) {
            return {devices_.at(*existing).protocol.handle(frame, now), std::nullopt};
        }
        auto found = devices_.find(name);
        if (found == devices_.end()) {
            if (devices_.size() >= limit_) {
                (*validation.ack)["result"] = 0;
                validation.events = {{{"event", "rejected"}, {"reason", "device_limit"}}};
                validation.close_after_send = true;
                return {std::move(validation), std::nullopt};
            }
            found = devices_.try_emplace(name, freshness_).first;
        }
        auto& device = found->second;
        if (device.current == token)
            return {device.protocol.handle(frame, now), std::nullopt};

        // 新连接接管时重置属性采集状态，并返回旧 token，交给 I/O 线程关闭旧 socket。
        const auto old = device.current;
        auto reply = device.protocol.handle(frame, now);
        device.protocol.restart_session();
        device.current = token;
        if (old != 0) bindings_.erase(old);
        bindings_[token] = name;
        if (reply.events.empty()) reply.events.push_back({{"event", "login"}, {"devId", name}});
        return {std::move(reply), old != 0 ? std::optional<Token>(old) : std::nullopt};
    }

    std::size_t limit_;
    std::chrono::milliseconds freshness_;
    // devices_ 保存设备档案，bindings_ 只保存当前有效连接到设备的映射。
    std::map<std::string, Device> devices_;
    std::map<Token, std::string> bindings_;
};
} // namespace batt
