/*
 * 文件作用：batt-sim 模拟终端入口，为服务端提供可复现的协议输入。
 * 每个 Terminal 拥有一条真实 TCP 连接；默认创建 BT000 起的 32 个模拟设备。
 * normal 演示登录、属性和告警生命周期；relogin 验证合法接管与无效登录隔离。
 * serve 持续上报，并在收到 910 后按配置延迟发送匹配 txnNo 的 911。
 * 本文件复用 JSON/切帧/网络工具，不运行服务端的 Protocol/Sessions/Queries 业务状态机。
 * 它用于功能演示；模拟数据与连接数量不能直接作为真实硬件或吞吐性能证据。
 */
#include "net.hpp"
#include "protocol.hpp"
#include <algorithm>
#include <csignal>
#include <deque>
#include <iomanip>
#include <iostream>
#include <memory>
#include <poll.h>
#include <sstream>

namespace {
volatile std::sig_atomic_t simulator_stop = 0;
extern "C" void stop_simulator(int) { simulator_stop = 1; }
std::string device_name(std::size_t index) {
    std::ostringstream out;
    out << "BT" << std::setw(3) << std::setfill('0') << index;
    return out.str();
}
// 以下函数手工组织终端侧报文，便于先从“线上到底发送什么”理解服务端分支。
batt::Json login(const std::string& dev, std::int64_t txn = 1) {
    return {{"msgType", 110}, {"devId", dev}, {"imsi", "SIM_" + dev}, {"txnNo", txn}, {"devType", 1}};
}
// 电压/电流按原协议放大 100 倍；额外未知浮点属性用于检查服务端能否正确忽略。
batt::Json attributes(const std::string& dev, std::int64_t txn, std::int64_t voltage = 5280) {
    return {{"msgType", 310}, {"devId", dev}, {"txnNo", txn},
        {"attrList", {{{"id", "01111001"}, {"value", voltage}},
                      {{"id", "01112001"}, {"value", -125}},
                      {{"id", "01113001"}, {"value", 80}},
                      {{"id", "01119001"}, {"value", 28}},
                      {{"id", "01102001"}, {"value", 120.12}}}}};
}
// 发送完整当前集合，空集合编码成 000/0；恢复单项通过下一帧集合缺少该项体现。
batt::Json alarm_frame(const std::string& dev, std::int64_t txn, const std::vector<std::string>& active) {
    batt::Json items = batt::Json::array();
    if (active.empty()) items.push_back({{"id", "01003001"}, {"alarmDesc", "000"}, {"alarmFlag", 0}});
    else for (const auto& desc : active)
        items.push_back({{"id", "01003001"}, {"alarmDesc", desc}, {"alarmFlag", 1}});
    return {{"msgType", 410}, {"devId", dev}, {"txnNo", txn}, {"alarmList", items}};
}

// 模拟器使用带发送超时的阻塞连接，读取时可用 MSG_DONTWAIT；并非服务端的 epoll 模型。
class Terminal {
public:
    Terminal(const std::string& host, unsigned short port, std::size_t slice, bool verbose,
             std::chrono::milliseconds query_delay = std::chrono::milliseconds(0))
        : socket_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)), slice_(slice), verbose_(verbose),
          query_delay_(query_delay) {
        if (!socket_) throw batt::sys_error("socket");
        auto addr = batt::address(host, port);
        if (::connect(socket_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            throw batt::sys_error("connect");
        timeval timeout{3, 0};
        if (::setsockopt(socket_.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0)
            throw batt::sys_error("send timeout");
    }
    // 即使人为限制 --slice，也必须根据 send 实际返回值推进；send 分片不保证 recv 同样分片。
    void send_only(const batt::Json& frame) {
        const auto wire = frame.dump();
        for (std::size_t pos = 0; pos < wire.size();) {
            const auto count = std::min(slice_, wire.size() - pos);
            const auto sent = ::send(socket_.get(), wire.data() + pos, count, MSG_NOSIGNAL);
            if (sent < 0) { if (errno == EINTR) continue; throw batt::sys_error("send"); }
            if (sent == 0) throw std::runtime_error("send returned zero");
            pos += static_cast<std::size_t>(sent);
        }
    }
    // 发送后校验 ACK 的类型、设备、编号和结果；等待期间若收到 910，先安排查询回复。
    void exchange(const batt::Json& frame, int expected, int result = 1) {
        if (frame.at("msgType") == 110) device_ = frame.at("devId").get<std::string>();
        send_only(frame);
        auto answer = response();
        while (answer.value("msgType", 0) == 910) { schedule(answer); answer = response(); }
        if (answer.value("msgType", 0) != expected || answer.value("result", -1) != result ||
            answer.at("devId") != frame.at("devId") || answer.at("txnNo") != frame.at("txnNo"))
            throw std::runtime_error("unexpected acknowledgement: " + answer.dump());
        if (verbose_) std::cout << batt::Json({{"event", "ack"}, {"message", answer}}).dump() << std::endl;
    }
    // 每轮推进收包、到期回复与周期上报；延迟 911 用 due 时间调度，不 sleep 阻塞其他设备。
    void serve_tick(std::int64_t voltage) {
        receive_available();
        if (peer_closed_) throw std::runtime_error("server closed connection");
        while (!inbox_.empty()) {
            auto frame = std::move(inbox_.front()); inbox_.pop_front();
            if (frame.value("msgType", 0) == 910) schedule(frame);
        }
        const auto now = batt::Clock::now();
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (now < it->due) { ++it; continue; }
            const auto id = it->request.at("txnNo");
            send_only({{"msgType", 911}, {"devId", device_}, {"txnNo", id},
                       {"attrList", {{{"id", "8001"}, {"protect", 4150}, {"recovery", 4100}, {"delay", 1000}}}}});
            std::cout << batt::Json({{"event", "query_response"}, {"devId", device_},
                                    {"txnNo", id}, {"delay_ms", query_delay_.count()}}).dump() << std::endl;
            it = pending_.erase(it);
        }
        if (now >= next_report_) {
            send_only(attributes(device_, ++report_txn_, voltage));
            send_only(alarm_frame(device_, ++report_txn_, {"03"}));
            next_report_ = now + std::chrono::seconds(1);
        }
    }
    bool expect_closed() {
        if (peer_closed_) return true;
        pollfd wait{socket_.get(), POLLIN, 0};
        int ready;
        do { ready = ::poll(&wait, 1, 3000); } while (ready < 0 && errno == EINTR);
        if (ready <= 0) return false;
        char byte;
        const auto n = ::recv(socket_.get(), &byte, 1, MSG_DONTWAIT);
        return n == 0 || (n < 0 && errno == ECONNRESET);
    }
private:
    // 保存原始 910 与计划回复时刻，911 原样回显服务器 txnNo，不能使用终端上报序号替代。
    void schedule(const batt::Json& frame) {
        if (frame.value("devId", std::string()) != device_ || !frame.contains("txnNo") ||
            !batt::integer(frame["txnNo"]) || pending_.size() >= 64) throw std::runtime_error("invalid query");
        pending_.push_back({frame, batt::Clock::now() + query_delay_});
    }
    // 和服务端一样用 JsonSlice 处理半帧/连续对象，但这里只把解析结果存入收件队列。
    void receive_available() {
        if (peer_closed_) return;
        char bytes[4096];
        for (unsigned budget = 0; budget < 16; ++budget) {
            const auto n = ::recv(socket_.get(), bytes, sizeof(bytes), MSG_DONTWAIT);
            if (n == 0) { peer_closed_ = true; return; }
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                if (errno == ECONNRESET) { peer_closed_ = true; return; }
                throw batt::sys_error("recv");
            }
            slice_parser_.feed(std::string_view(bytes, static_cast<std::size_t>(n)), batt::Clock::now(),
                [&](std::string&& text) {
                    auto value = batt::parse_object(text);
                    if (!value || inbox_.size() >= 256) throw std::runtime_error("invalid server stream");
                    inbox_.push_back(std::move(*value));
                });
        }
    }
    // 等待一个完整服务端对象，poll 提供剩余超时等待；收到 EOF 或超过 3 秒视为演示失败。
    batt::Json response() {
        const auto end = batt::Clock::now() + std::chrono::seconds(3);
        for (;;) {
            if (!inbox_.empty()) { auto value = std::move(inbox_.front()); inbox_.pop_front(); return value; }
            if (peer_closed_) throw std::runtime_error("server closed connection");
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(end - batt::Clock::now()).count();
            if (remaining <= 0) throw std::runtime_error("ack timeout");
            pollfd wait{socket_.get(), POLLIN, 0};
            const int ready = ::poll(&wait, 1, static_cast<int>(remaining));
            if (ready < 0) { if (errno == EINTR) continue; throw batt::sys_error("poll"); }
            if (ready == 0) continue;
            receive_available();
        }
    }
    batt::Fd socket_;
    std::size_t slice_;
    bool verbose_;
    batt::JsonSlice slice_parser_;
    std::string device_;
    std::deque<batt::Json> inbox_;
    bool peer_closed_ = false;
    struct Scheduled { batt::Json request; batt::Time due; };
    std::vector<Scheduled> pending_;
    std::chrono::milliseconds query_delay_;
    batt::Time next_report_ = batt::Clock::now() + std::chrono::seconds(1);
    std::int64_t report_txn_ = 100;
};

// 依次证明无效登录不影响旧连接、合法登录替换旧连接、其他设备仍可正常通信。
void relogin_demo(const std::string& host, unsigned short port, std::size_t slice) {
    Terminal old(host, port, slice, true);
    old.exchange(login("BT000"), 111);
    old.exchange(attributes("BT000", 2), 311);
    Terminal other(host, port, slice, true);
    other.exchange(login("BT001"), 111);
    other.exchange(attributes("BT001", 2, 6100), 311);
    Terminal invalid(host, port, slice, true);
    auto bad = login("BT000", 3); bad["devType"] = 2;
    invalid.exchange(bad, 111, 0);
    if (!invalid.expect_closed()) throw std::runtime_error("invalid login was not closed");
    old.exchange(attributes("BT000", 4), 311);
    Terminal next(host, port, slice, true);
    next.exchange(login("BT000", 5), 111);
    try { old.send_only(attributes("BT000", 6, 9999)); } catch (const std::runtime_error&) {}
    if (!old.expect_closed()) throw std::runtime_error("replaced connection still usable");
    next.exchange(attributes("BT000", 7, 6500), 311);
    next.exchange(login("BT000", 8), 111);
    other.exchange(attributes("BT001", 9, 6100), 311);
    std::cout << "{\"event\":\"relogin_complete\",\"old_connection_blocked\":true,\"other_device_ok\":true}\n";
}
} // namespace

// 解析模拟参数后选择 normal/relogin/serve；serve 可一直运行，也可指定持续时间。
int main(int argc, char** argv) {
    try {
        std::string host = "127.0.0.1";
        std::string demo = "normal";
        unsigned short port = 19001;
        std::size_t slice = 64 * 1024;
        std::size_t devices = 32;
        std::size_t hold_seconds = 1;
        std::size_t duration_seconds = 0;
        std::chrono::milliseconds query_delay{0};
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "batt-sim [--host IPv4] [--port 19001] [--slice N] [--devices 32]\n"
                             "         [--hold-seconds 1] [--demo normal|relogin|serve]\n"
                             "         [--query-delay-ms 0] [--duration-seconds 0]\n";
                return 0;
            }
            if (i + 1 >= argc) throw std::invalid_argument("missing option value");
            const std::string value = argv[++i];
            if (arg == "--host") { host = value; continue; }
            if (arg == "--demo") { demo = value; continue; }
            if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                throw std::invalid_argument("nonnegative integer required");
            const auto n = std::stoul(value);
            if (arg == "--port") {
                if (n == 0 || n > 65535) throw std::invalid_argument("port out of range");
                port = static_cast<unsigned short>(n);
            } else if (arg == "--slice") {
                if (n == 0 || n > 64 * 1024) throw std::invalid_argument("slice must be 1..65536");
                slice = n;
            } else if (arg == "--devices") {
                if (n == 0 || n > 1024) throw std::invalid_argument("devices must be 1..1024");
                devices = n;
            } else if (arg == "--hold-seconds") {
                if (n > 300) throw std::invalid_argument("hold must be 0..300 seconds");
                hold_seconds = n;
            } else if (arg == "--query-delay-ms") {
                if (n > 10000) throw std::invalid_argument("query delay must be 0..10000 ms");
                query_delay = std::chrono::milliseconds(static_cast<std::int64_t>(n));
            } else if (arg == "--duration-seconds") {
                if (n > 3600) throw std::invalid_argument("duration must be 0..3600 seconds");
                duration_seconds = n;
            } else throw std::invalid_argument("unknown option: " + arg);
        }
        if (demo == "relogin") { relogin_demo(host, port, slice); return 0; }
        if (demo != "normal" && demo != "serve") throw std::invalid_argument("unknown demo");
        std::vector<std::unique_ptr<Terminal>> terminals;
        // 先建立全部终端连接并完成登录，再进行上报，便于观察多设备同时存在时的状态隔离。
        for (std::size_t i = 0; i < devices; ++i) {
            auto terminal = std::make_unique<Terminal>(host, port, slice, devices == 1, query_delay);
            terminal->exchange(login(device_name(i)), 111);
            terminals.push_back(std::move(terminal));
        }
        std::cout << batt::Json({{"event", "all_connected"}, {"devices", devices}}).dump() << std::endl;
        if (demo == "serve") {
            struct sigaction action{}; action.sa_handler = stop_simulator; ::sigemptyset(&action.sa_mask);
            ::sigaction(SIGINT, &action, nullptr); ::sigaction(SIGTERM, &action, nullptr);
            for (std::size_t i = 0; i < devices; ++i) {
                terminals[i]->exchange(attributes(device_name(i), 2, 5280 + static_cast<std::int64_t>(i)), 311);
                terminals[i]->exchange(alarm_frame(device_name(i), 3, {"03"}), 411);
            }
            std::cout << batt::Json({{"event", "serving"}, {"devices", devices}, {"query_delay_ms", query_delay.count()}}).dump() << std::endl;
            const auto started = batt::Clock::now();
            while (!simulator_stop && (duration_seconds == 0 ||
                   batt::Clock::now() - started < std::chrono::seconds(duration_seconds))) {
                for (std::size_t i = 0; i < devices; ++i) terminals[i]->serve_tick(5280 + static_cast<std::int64_t>(i));
                ::poll(nullptr, 0, 10);
            }
            std::cout << "{\"event\":\"serve_stopped\"}\n";
            return 0;
        }
        for (std::size_t i = 0; i < devices; ++i) {
            auto& terminal = *terminals[i];
            const auto name = device_name(i);
            terminal.exchange(attributes(name, 2, 5280 + static_cast<std::int64_t>(i)), 311);
            // 本段依次测试持续、增加、减少、全清和再触发，重复 03 应保持同一 open 事件。
            terminal.exchange(alarm_frame(name, 3, {"03"}), 411);
            terminal.exchange(alarm_frame(name, 4, {"03"}), 411);
            terminal.exchange(alarm_frame(name, 5, {"01", "03"}), 411);
            terminal.exchange(alarm_frame(name, 6, {"01"}), 411);
            terminal.exchange(alarm_frame(name, 7, {}), 411);
            terminal.exchange(alarm_frame(name, 8, {"03"}), 411);
            if (devices > 1) std::cout << batt::Json({{"event", "device_complete"}, {"devId", name}, {"acks", 8}}).dump() << std::endl;
        }
        ::poll(nullptr, 0, static_cast<int>(hold_seconds * 1000));
        std::cout << batt::Json({{"event", "demo_complete"}, {"devices", devices}, {"acks", devices * 8}}).dump() << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "batt-sim: " << error.what() << '\n';
        return 1;
    }
}
