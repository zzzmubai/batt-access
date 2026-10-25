/*
 * 文件作用：将结构化业务事件转换成适合交互终端阅读的中文输出。
 * 调用者是 access.cpp::log；verbose 模式直接打印 JSON，quiet 模式使用本文件格式化。
 * 高频属性/告警事件在安静模式被隐藏，status/query/task 等命令结果仍会显示。
 * 这里仅决定展示内容，不改变业务处理、数据库提交或网络报文。
 */
#pragma once
#include "protocol.hpp"
#include <iomanip>
#include <sstream>

namespace batt {
// 用 JSON 转义处理设备编号中的控制字符，避免它们被终端当成换行或控制指令执行。
inline std::string display_label(const std::string& text) {
    const auto encoded = Json(text).dump();
    return encoded.substr(1, encoded.size() - 2); // Escape terminal control characters.
}
inline std::string state_label(const std::string& state) {
    if (state == "online_fresh") return "在线，数据新鲜";
    if (state == "connected_no_data") return "已连接，数据尚未齐全";
    if (state == "connected_stale") return "连接仍在，数据已过期";
    if (state == "offline") return "已断线";
    if (state == "pending") return "等待终端应答";
    if (state == "success") return "查询成功";
    if (state == "timeout") return "查询超时";
    if (state == "link_lost") return "连接丢失，查询结束";
    return state;
}
// 缺少属性显示 --；已有数值按指定小数位显示，不把缺失值误显示成 0。
inline std::string number_label(const Json& event, const std::string& key, int digits) {
    if (!event.contains(key) || !event[key].is_number()) return "--";
    std::ostringstream out;
    out << std::fixed << std::setprecision(digits) << event[key].get<double>();
    return out.str();
}

// Only presentation changes here; processing and JSON mode remain unchanged.
// 返回 nullopt 表示隐藏该事件；返回字符串表示本次要打印的文本。
inline std::optional<std::string> quiet_console_line(const Json& event) {
    const auto kind = event.value("event", std::string());
    if (kind == "attributes" || kind == "alarms" || kind == "accepted" ||
        kind == "login_queued" || kind == "login" || kind == "device_state") return std::nullopt;
    const auto dev = display_label(event.value("devId", std::string()));
    const auto state = event.value("state", std::string());
    const auto id = event.contains("txnNo") ? event["txnNo"].dump() : "?";
    if (kind == "status" || kind == "device") {
        return std::string(kind == "status" ? "[STATUS] " : "[DEVICE] ") + dev + " | " + state +
            "（" + state_label(state) + "） | 电压=" + number_label(event, "voltage_v", 2) +
            "V | 电流=" + number_label(event, "current_a", 2) + "A | SOC=" +
            number_label(event, "soc", 0) + "% | 温度=" + number_label(event, "temperature_c", 0) +
            "C | worker=" + event.value("worker", Json(-1)).dump();
    }
    if (kind == "query_state" || kind == "task") {
        std::string out = std::string(kind == "task" ? "[TASK #" : "[QUERY #") + id + "] " + dev +
            " | " + state + "（" + state_label(state) + "）";
        const auto reason = event.value("reason", std::string());
        if (!reason.empty()) out += " | reason=" + display_label(reason);
        if (state == "success" && event.contains("response") && !event["response"].is_null())
            out += "\n[RESULT #" + id + "] " + event["response"].dump();
        return out;
    }
    if (kind == "late_response") return "[LATE #" + id + "] " + dev + " 收到迟到应答，状态仍为timeout。";
    if (kind == "query_rejected") return "[QUERY] " + dev + " 未受理 | reason=" + event.value("reason", std::string());
    if (kind == "not_found") return "[STATUS] 未找到设备 " + dev;
    if (kind == "task_not_found") return "[TASK #" + id + "] 未找到任务。";
    if (kind == "command") return "> " + display_label(event.value("input", std::string()));
    if (kind == "console_mode") return "[CONSOLE] 安静模式已开启：只显示命令结果、查询变化和重要事件。";
    if (kind == "help") return "[HELP] status BT000 | query BT000 | task 编号 | devices | alarms BT000 | stats | quiet on | quiet off | quit";
    return event.dump();
}
} // namespace batt
