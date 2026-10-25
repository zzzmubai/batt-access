/*
 * 文件作用：batt-access 服务端入口，负责网络 I/O 与业务线程之间的调度。
 * 主线程独占监听 socket、epoll、各连接的收发缓冲和控制台输入。
 * 收包主线：receive -> JsonSlice::feed -> process_object -> worker 输入队列。
 * 返回主线：drain_outputs -> SendBuffer::enqueue -> send_pending -> socket send。
 * 业务状态由固定设备分片 worker 管理；运行中的 I/O 循环不执行 SQLite 操作。
 * 本文件还处理连接替换、容量限制、登录/残帧/发送期限和正常退出。
 * 建议先读 protocol/session/worker，再从 main 和 Server::run 串起完整链路。
 */
#include "net.hpp"
#include "worker.hpp"
#include "send_buffer.hpp"
#include "console.hpp"
#include "http_api.hpp"
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <sys/epoll.h>
#include <sys/file.h>

namespace {
// 信号处理器只设置无锁停止标志；数据库、日志和退出排空由主循环完成。
std::atomic<bool> stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free, "signal notification must be lock-free");
extern "C" void request_stop(int) { stop_requested.store(true, std::memory_order_relaxed); }
constexpr batt::Token stdin_tag = std::numeric_limits<batt::Token>::max();
void print_json(const batt::Json& value) { std::cout << value.dump() << std::endl; }
// 每次进程启动生成一份身份，与递增 token 拼接，避免跨进程混淆连接记录。
std::string boot_identity() {
    std::ifstream source("/proc/sys/kernel/random/uuid"); std::string value;
    if (!(source >> value)) throw std::runtime_error("cannot obtain boot identity");
    return value;
}
// 仅由 I/O 线程访问的连接上下文；fd 用于系统调用，token 用于异步消息关联。
struct Connection {
    batt::Fd fd;
    batt::Token token;
    batt::JsonSlice slice;
    batt::SendBuffer outgoing;
    batt::Time accepted = batt::Clock::now(), last_progress = accepted;
    batt::Json last_snapshot = {{"state", "connected_unauthed"}};
    std::optional<std::size_t> owner;
    std::string device, closing;
    // 登录尚待 worker 确认时暂停后续读取；同批 recv 中已切出的对象暂存到 held。
    bool login_inflight = false, drop_now = false;
    // inflight 统计已投递但尚未返回的帧；held_bytes 统计尚未受理的完整对象字节。
    std::size_t inflight = 0, held_bytes = 0;
    std::deque<std::string> held;
    unsigned consecutive_bad = 0;
    std::uint64_t bad_json = 0;
    Connection(batt::Fd socket, batt::Token id) : fd(std::move(socket)), token(id) {}
};
// 队列满时暂存断开通知并重试，不能静默丢失，也不能阻塞 I/O 等待空位。
struct DeferredClose { std::size_t shard; batt::Work work; };

// 采用 epoll LT（默认水平触发）；每轮接收/发送都有预算，避免单连接饿死其他连接。
class Server {
public:
    // workers_ 构造会完成数据库初始化和线程恢复，随后构造函数体才 bind/listen。
    Server(const std::string& host, unsigned short port, std::size_t chunk,
           std::size_t max_clients, const batt::WorkerConfig& config, bool quiet, const batt::HttpOptions& http_options)
        : listener_(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)),
          epoll_(::epoll_create1(EPOLL_CLOEXEC)), workers_(config), api_bridge_(http_options.application_wait,http_options.command_capacity),
          registered_(config.workers, 0), send_chunk_(chunk), max_clients_(max_clients),
          boot_(config.storage.boot_id), quiet_(quiet) {
        if (!listener_ || !epoll_) throw batt::sys_error("socket/epoll_create1");
        const int yes = 1;
        if (::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
            throw batt::sys_error("setsockopt");
        auto addr = batt::address(host, port);
        if (::bind(listener_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
            ::listen(listener_.get(), 128) < 0) throw batt::sys_error("bind/listen");
        socklen_t len = sizeof(addr);
        if (::getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&addr), &len) < 0)
            throw batt::sys_error("getsockname");
        epoll_event event{}; event.events = EPOLLIN; event.data.u64 = 0;
        if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, listener_.get(), &event) < 0)
            throw batt::sys_error("epoll add listener");
        // 把标准输入也加入 epoll：CLI 命令是正在运行的服务输入，不是独立 shell 命令。
        stdin_flags_ = ::fcntl(STDIN_FILENO, F_GETFL);
        if (stdin_flags_ >= 0 && ::fcntl(STDIN_FILENO, F_SETFL, stdin_flags_ | O_NONBLOCK) == 0) {
            event.events = EPOLLIN; event.data.u64 = stdin_tag;
            cli_enabled_ = ::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, STDIN_FILENO, &event) == 0;
        }
        if (http_options.enabled) http_=std::make_unique<batt::HttpGateway>(api_bridge_,http_options);
        log({{"event", "ready"}, {"host", host}, {"port", ntohs(addr.sin_port)},
             {"milestone", 4}, {"workers", workers_.count()}, {"max_clients", max_clients_},
             {"boot_id", boot_}, {"freshness_ms", config.freshness.count()},
             {"query_timeout_ms", config.query_timeout.count()}, {"cli", cli_enabled_},
             {"persistence", "sqlite"}, {"database", config.storage.path},
             {"sqlite_version", sqlite3_libversion()}, {"quiet", quiet_}, {"version","2.0.0"},
             {"http_port",http_ ? batt::Json(http_->port()) : batt::Json(nullptr)}});
        if (quiet_) log({{"event", "console_mode"}, {"quiet", true}});
    }
    ~Server() { if (http_) http_->stop(); if (stdin_flags_ >= 0) ::fcntl(STDIN_FILENO, F_SETFL, stdin_flags_); }

    // 先消费业务结果和维护期限，再等待网络事件；epoll 最多等待 10ms 以定期收取 worker 输出。
    void run() {
        for (;;) {
            drain_outputs();
            drain_api();
            if (workers_.failed()) {
                unfinished("storage_failure");
                throw batt::StorageError("worker storage failed; service stopped");
            }
            if (stop_requested && !stopping_) begin_stop();
            retry_closes();
            maintain(batt::Clock::now());
            // 退出时继续排空已受理任务、结果队列和发送缓存；超过 5 秒记录未完成量并失败退出。
            if (stopping_) {
                if (!inputs_finished_ && deferred_.empty()) { workers_.finish(); inputs_finished_ = true; }
                bool sends_empty = true;
                for (const auto& item : connections_) if (!item.second->outgoing.empty()) sends_empty = false;
                if (workers_.done() && workers_.output_empty() && sends_empty) break;
                if (batt::Clock::now() - shutdown_at_ >= std::chrono::seconds(5)) {
                    unfinished("shutdown_timeout");
                    workers_.abort(); throw std::runtime_error("shutdown_timeout");
                }
            }
            epoll_event events[64]{};
            const int count = ::epoll_wait(epoll_.get(), events, 64, 10);
            if (count < 0) { if (errno == EINTR) continue; throw batt::sys_error("epoll_wait"); }
            for (int i = 0; i < count; ++i) {
                // epoll 事件携带 token 而非 fd；本批旧事件即使遇到 fd 复用，也无法命中新连接。
                const auto token = events[i].data.u64;
                if (token == 0) { if (!stopping_ && listener_) accept_clients(); continue; }
                if (token == stdin_tag) { if (!stopping_) read_commands(); continue; }
                auto* client = connection(token);
                if (!client) { ++stale_events_; continue; }
                if ((events[i].events & EPOLLERR) != 0) { close_connection(token, "socket_error"); continue; }
                if (!stopping_ && client->closing.empty() && !client->login_inflight &&
                    (events[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) != 0) receive(token);
                client = connection(token);
                if (client && !client->outgoing.empty()) send_pending(token);
            }
        }
        while (!connections_.empty()) close_connection(connections_.begin()->first, "server_shutdown");
        log({{"event", "stopped"}, {"bad_json", total_bad_}, {"peak_clients", peak_clients_},
             {"registered_devices", std::accumulate(registered_.begin(), registered_.end(), std::size_t(0))},
             {"replacements", replacements_}, {"stale_events", stale_events_},
             {"stale_results", stale_results_}, {"overloaded", overloaded_}});
        if (http_) log({{"event","http_stats"},{"bridge",api_bridge_.stats()},{"http",http_->stats()}});
        if (workers_.failed()) throw std::runtime_error("worker_failed");
    }

private:
    void drain_api() {
        for (unsigned budget=0; budget<16; ++budget) {
            auto call=api_bridge_.take(); if (!call) break;
            if (call->context->expired()) continue;
            if (stopping_) { call->context->complete(batt::api_error(503,"server_stopping","Retry same request")); continue; }
            batt::Work work; work.kind=batt::WorkKind::command; work.device=call->command.device;
            work.action=call->command; work.api=call->context;
            const auto shard=call->command.kind==batt::CommandKind::task ? 0 : batt::device_shard(call->command.device,workers_.count());
            if (!workers_.submit(shard,std::move(work))) {
                auto response=batt::api_error(503,"worker_busy","Command not admitted");
                if (call->context->post) response.body["outcome"]="not_created";
                call->context->complete(std::move(response));
            }
        }
    }
    // 失败时记录待发字节、未受理字节和队列状态，便于区分哪些数据没有完成保证。
    void unfinished(const std::string& reason) {
        std::size_t unsent = 0, unaccepted = 0;
        for (const auto& item : connections_) {
            unsent += item.second->outgoing.size();
            unaccepted += item.second->held_bytes + item.second->slice.buffered();
        }
        log({{"event", "unfinished"}, {"reason", reason}, {"unsent_bytes", unsent},
             {"unaccepted_bytes", unaccepted}, {"deferred_closes", deferred_.size()}, {"queues", workers_.queues()}});
    }
    void log(const batt::Json& event) const {
        if (!quiet_ || event.value("event", std::string()) == "ready") { print_json(event); return; }
        if (const auto line = batt::quiet_console_line(event)) std::cout << *line << std::endl;
    }
    Connection* connection(batt::Token token) {
        const auto found = connections_.find(token);
        return found == connections_.end() ? nullptr : found->second.get();
    }
    std::string identity(batt::Token token) const { return boot_ + ":" + std::to_string(token); }
    // 停止监听和新输入，丢弃尚未投递的 held；已经进入 worker 队列的任务继续排空。
    void begin_stop() {
        if (http_) http_->stop();
        stopping_ = true; shutdown_at_ = batt::Clock::now(); listener_.reset();
        if (cli_enabled_) ::epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, STDIN_FILENO, nullptr);
        cli_enabled_ = false;
        std::size_t discarded = 0;
        for (const auto& item : connections_) discarded += item.second->held_bytes + item.second->slice.buffered();
        log({{"event", "stopping"}, {"reason", "requested"}, {"unaccepted_bytes", discarded}});
        for (auto& item : connections_) { item.second->held.clear(); item.second->held_bytes = 0; }
    }
    // 关闭通知按原 token 重试投递；Sessions 会过滤已被替换的旧会话通知。
    void retry_closes() {
        for (auto it = deferred_.begin(); it != deferred_.end();) {
            if (workers_.submit(it->shard, it->work)) it = deferred_.erase(it);
            else ++it;
        }
    }
    // 检查登录、残帧、发送无进展的 5 秒期限；这与业务数据 90 秒过期、查询 3 秒不同。
    void maintain(batt::Time now) {
        for (auto it = connections_.begin(); it != connections_.end();) {
            const auto token = it->first; auto* client = it->second.get(); ++it;
            if (client->drop_now) close_connection(token, client->closing);
            else if (!stopping_ && client->device.empty() && now - client->accepted >= std::chrono::seconds(5))
                close_connection(token, "login_timeout");
            else if (!stopping_ && client->slice.expired(now, std::chrono::seconds(5)))
                close_connection(token, "frame_timeout");
            else if (!client->outgoing.empty() && now - client->last_progress >= std::chrono::seconds(5))
                close_connection(token, "send_timeout");
            client = connection(token);
            if (client && !client->closing.empty() && client->outgoing.empty() && client->inflight == 0)
                close_connection(token, client->closing);
            if (connection(token)) update_interest(token);
        }
    }
    // accept4 直接建立非阻塞连接，分配全新 token；连接和等待关闭通知共同计入上限。
    void accept_clients() {
        for (unsigned accepted = 0; accepted < 32; ++accepted) {
            const int fd = ::accept4(listener_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                throw batt::sys_error("accept4");
            }
            batt::Fd guard(fd);
            if (connections_.size() + deferred_.size() >= max_clients_) {
                log({{"event", "connection_rejected"}, {"reason", "connection_limit"}}); continue;
            }
            if (next_token_ >= stdin_tag - 1) throw std::runtime_error("connection_token_exhausted");
            const auto token = ++next_token_;
            auto client = std::make_unique<Connection>(std::move(guard), token);
            epoll_event event{}; event.data.u64 = token; event.events = EPOLLIN | EPOLLRDHUP;
            if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd, &event) < 0) throw batt::sys_error("epoll add client");
            connections_.emplace(token, std::move(client));
            peak_clients_ = std::max(peak_clients_, connections_.size());
            log({{"event", "accepted"}, {"connection_id", identity(token)}, {"fd", fd},
                 {"active_clients", connections_.size()}});
        }
    }
    // 先处理完整对象的 JSON 语法和基本路由字段；业务校验由目标 worker 中的状态机完成。
    void process_object(batt::Token token, std::string object) {
        auto* client = connection(token);
        if (!client || !client->closing.empty() || stopping_) return;
        // 登录 ACK 尚未形成时，同批字节中后续对象不能越过登录处理；暂存也有数量/字节上限。
        if (client->login_inflight) {
            if (client->held.size() >= 64 || client->held_bytes + object.size() > 256 * 1024) {
                client->closing = "held_overflow"; client->drop_now = true; return;
            }
            client->held_bytes += object.size(); client->held.push_back(std::move(object)); return;
        }
        // 边界完整但 JSON 语法错误时可丢弃这一对象；连续三次坏 JSON 请求关闭连接。
        auto frame = batt::parse_object(object);
        if (!frame) {
            ++total_bad_; ++client->bad_json; ++client->consecutive_bad;
            log({{"event", "bad_json"}, {"connection_id", identity(token)}});
            if (client->consecutive_bad >= 3) client->closing = "consecutive_bad_json";
            return;
        }
        client->consecutive_bad = 0;
        batt::Work work; work.token = token; work.frame = std::move(*frame);
        work.login_deadline = client->accepted + std::chrono::seconds(5);
        if (client->device.empty()) {
            const bool login = work.frame.contains("msgType") &&
                batt::integer(work.frame["msgType"]) == std::optional<std::int64_t>(110);
            if (!login || !work.frame.contains("devId") || !batt::identifier(work.frame["devId"])) {
                batt::Protocol unbound; auto rejected = unbound.handle(work.frame, batt::Clock::now());
                for (auto event : rejected.events) { event["connection_id"] = identity(token); log(event); }
                return;
            }
            work.device = work.frame["devId"].get<std::string>();
            work.initial_login = true;
            // 首次登录根据 devId 固定分片，之后沿用连接所属分片，避免伪造字段改变路由。
            client->owner = batt::device_shard(work.device, workers_.count());
            client->login_inflight = true;
        } else work.device = client->device;
        const bool first_login = work.initial_login;
        // 首版队列满采用关闭该连接的卸载策略；这里不实现高低水位暂停/恢复。
        if (!workers_.submit(*client->owner, std::move(work))) {
            ++overloaded_; client->closing = "overloaded"; client->drop_now = true;
        } else {
            ++client->inflight;
            if (first_login) log({{"event", "login_queued"}, {"connection_id", identity(token)}, {"worker", *client->owner}});
        }
    }
    // 登录确认后按原顺序投递暂存对象；对象投递前仍属于未受理数据。
    void drain_held(batt::Token token) {
        auto* client = connection(token);
        while (client && !client->held.empty() && !client->login_inflight && client->closing.empty() && !stopping_) {
            auto text = std::move(client->held.front()); client->held.pop_front(); client->held_bytes -= text.size();
            process_object(token, std::move(text)); client = connection(token);
        }
    }
    // 把 worker 的业务结果落实到 I/O：绑定、替换、日志、发送和关闭请求都在本线程处理。
    void drain_outputs() {
        for (unsigned budget = 0; budget < 256; ++budget) {
            auto next = workers_.take(); if (!next) break;
            auto& out = *next; registered_.at(out.worker) = out.registered;
            auto* client = connection(out.token);
            if (out.frame_done && client && client->inflight > 0) --client->inflight;
            if (out.bind && client) {
                client->device = *out.bind; client->owner = out.worker;
                current_[*out.bind] = out.token; client->login_inflight = false;
            }
            if (out.replaced) {
                ++replacements_;
                log({{"event", "session_replaced"}, {"devId", out.bind.value_or("")},
                     {"old_connection_id", identity(*out.replaced)}, {"connection_id", identity(out.token)}});
                close_connection(*out.replaced, "replaced");
            }
            if (client && out.snapshot) client->last_snapshot = *out.snapshot;
            for (auto event : out.logs) {
                auto token = out.token;
                if (event.contains("session_token")) token = event["session_token"].get<batt::Token>();
                if (!event.contains("connection_id"))
                    event["connection_id"] = token ? batt::Json(identity(token)) : batt::Json(nullptr);
                log(event);
            }
            if (out.message) {
                bool allowed = client != nullptr;
                const int type = out.message->value("msgType", 0);
                // 发给设备的普通应答和查询再次核对当前 token，防止旧会话结果误发给新连接。
                if (allowed && type != 111) {
                    const auto dev = out.message->value("devId", std::string());
                    const auto active = current_.find(dev);
                    allowed = !dev.empty() && active != current_.end() && active->second == out.token;
                }
                if (type == 910 && stopping_) allowed = false;
                if (allowed) {
                    try {
                        if (client->outgoing.empty()) client->last_progress = batt::Clock::now();
                        client->outgoing.enqueue(out.message->dump(), out.deadline);
                    } catch (const std::exception& error) {
                        client->closing = error.what(); client->drop_now = true;
                    }
                } else ++stale_results_;
            }
            if (client && !out.close_reason.empty()) {
                client->login_inflight = false; client->closing = out.close_reason;
                client->held.clear(); client->held_bytes = 0;
            }
            if (out.bind && client && !stopping_) drain_held(out.token);
        }
    }
    // recv 的块大小与 JSON 报文无关；读到 EAGAIN 正常返回，残帧状态由 JsonSlice 保存。
    void receive(batt::Token token) {
        char bytes[4096]; std::size_t budget = 64 * 1024;
        while (budget > 0 && !stop_requested) {
            auto* client = connection(token);
            if (!client || !client->closing.empty() || client->login_inflight) return;
            const auto n = ::recv(client->fd.get(), bytes, sizeof(bytes), 0);
            if (n == 0) { client->closing = "peer_eof"; return; }
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                close_connection(token, "recv_error"); return;
            }
            budget -= std::min(budget, static_cast<std::size_t>(n));
            try {
                client->slice.feed(std::string_view(bytes, static_cast<std::size_t>(n)), batt::Clock::now(),
                    [&](std::string&& text) { process_object(token, std::move(text)); });
            } catch (const std::exception& error) { close_connection(token, error.what()); return; }
        }
    }
    // MSG_NOSIGNAL 避免对端断开时 SIGPIPE 终止进程；短写和 EAGAIN 由 SendBuffer 保留进度。
    void send_pending(batt::Token token) {
        auto* client = connection(token); if (!client) { ++stale_events_; return; }
        const auto before = client->outgoing.size();//记录发送前，发送缓冲区（outgoing）里积压的字节数。
        const auto result = client->outgoing.flush([&](const char* data, std::size_t size) {
            return static_cast<std::ptrdiff_t>(::send(client->fd.get(), data, size, MSG_NOSIGNAL));
        }, send_chunk_);
        if (client->outgoing.size() < before) client->last_progress = batt::Clock::now();
        if (result == batt::Flush::disconnected) close_connection(token, "send_error");
        else if (result == batt::Flush::deadline_partial) close_connection(token, "partial_query_expired");
    }
    // 仅有待发字节时订阅 EPOLLOUT，避免 socket 一直可写导致空转；登录处理中暂去掉 EPOLLIN。
    void update_interest(batt::Token token) {
        auto* client = connection(token); if (!client) return;
        epoll_event event{}; event.data.u64 = token; event.events = EPOLLRDHUP;
        if (!stopping_ && client->closing.empty() && !client->login_inflight) event.events |= EPOLLIN;
        if (!client->outgoing.empty()) event.events |= EPOLLOUT;
        if (::epoll_ctl(epoll_.get(), EPOLL_CTL_MOD, client->fd.get(), &event) < 0) throw batt::sys_error("epoll mod client");
    }
    // 从 epoll/连接表移除并让 Fd 析构关闭 socket，同时给所属 worker 投递断开通知。
    void close_connection(batt::Token token, std::string reason) {
        const auto found = connections_.find(token);
        if (found == connections_.end()) { ++stale_events_; return; }
        auto& client = *found->second;
        const auto bound = current_.find(client.device);
        const bool was_current = bound != current_.end() && bound->second == token;
        if (was_current) current_.erase(bound);
        log({{"event", "closed"}, {"connection_id", identity(token)}, {"fd", client.fd.get()},
             {"reason", reason}, {"was_current", was_current}, {"snapshot", client.last_snapshot},
             {"open_alarms", client.last_snapshot.value("open_alarms", 0)},
             {"alarm_history", client.last_snapshot.value("alarm_history", 0)}, {"bad_json", client.bad_json}});
        if (client.owner && !inputs_finished_) {
            batt::Work work; work.kind = batt::WorkKind::disconnected; work.token = token;
            work.device = client.device; work.reason = reason;
            if (!workers_.submit(*client.owner, work)) deferred_.push_back({*client.owner, std::move(work)});
        }
        ::epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, client.fd.get(), nullptr);
        connections_.erase(found);
    }

    void submit_command(std::size_t shard, batt::Work work) {
        if (!workers_.submit(shard, std::move(work))) log({{"event", "cli_busy"}, {"worker", shard}});
    }
    // 按命令路由：设备命令固定分片，devices/stats 广播，task 从 worker 0 的数据库连接查询。
    void dispatch_command(const std::string& line) {
        std::istringstream input(line); std::string verb, arg, extra;
        input >> verb; if (verb.empty()) return;
        input >> arg; input >> extra;
        if (verb == "quiet" && (arg == "on" || arg == "off") && extra.empty()) {
            quiet_ = arg == "on";
            log({{"event", "console_mode"}, {"quiet", quiet_}}); return;
        }
        log({{"event", "command"}, {"input", line}});
        if (verb == "quit" && arg.empty()) { stop_requested = 1; return; }
        if (verb == "help") {
            log({{"event", "help"}, {"commands", "devices | status DEV | query DEV | task ID | alarms DEV | stats | quiet on|off | quit"}}); return;
        }
        batt::Work work; work.kind = batt::WorkKind::command; work.command = verb;
        if ((verb == "devices" || verb == "stats") && arg.empty()) {
            if (verb == "stats") log({{"event", "stats"}, {"active_clients", connections_.size()},
                                      {"overloaded", overloaded_}, {"queues", workers_.queues()},
                                      {"deferred_closes", deferred_.size()}, {"stale_results", stale_results_}});
            if (verb=="stats" && http_) log({{"event","http_stats"},{"bridge",api_bridge_.stats()},{"http",http_->stats()}});
            for (std::size_t i = 0; i < workers_.count(); ++i) submit_command(i, work);
        } else if ((verb == "status" || verb == "query" || verb == "alarms") &&
                   !arg.empty() && arg.size() <= 64 && extra.empty()) {
            work.device = arg; submit_command(batt::device_shard(arg, workers_.count()), std::move(work));
        } else if (verb == "task" && !arg.empty() && extra.empty()) {
            try {
                if (arg.find_first_not_of("0123456789") != std::string::npos) throw std::invalid_argument("task");
                work.task_id = std::stoll(arg);
                submit_command(0, std::move(work));
            } catch (const std::exception&) { log({{"event", "cli_error"}, {"reason", "invalid_task_id"}}); }
        } else log({{"event", "cli_error"}, {"reason", "use_help"}});
    }
    // 非阻塞读取按换行分隔的 CLI 文本；单行上限 4096 字节，避免无换行输入无限积累。
    void read_commands() {
        char bytes[1024];
        for (unsigned budget = 0; budget < 8; ++budget) {
            const auto n = ::read(STDIN_FILENO, bytes, sizeof(bytes));
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            }
            if (n <= 0) {
                ::epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, STDIN_FILENO, nullptr); cli_enabled_ = false; return;
            }
            for (std::ptrdiff_t i = 0; i < n; ++i) {
                if (bytes[i] == '\n') { dispatch_command(command_line_); command_line_.clear(); }
                else if (bytes[i] != '\r') command_line_.push_back(bytes[i]);
                if (command_line_.size() > 4096) { command_line_.clear(); log({{"event", "cli_error"}, {"reason", "line_too_long"}}); }
                if (stop_requested) return;
            }
        }
    }

    batt::Fd listener_, epoll_;
    batt::Workers workers_;
    batt::ApiBridge api_bridge_;
    std::unique_ptr<batt::HttpGateway> http_;
    std::vector<std::size_t> registered_;
    // connections_ 是 I/O 连接表，current_ 是发送路由索引；设备正式业务状态由 worker 持有。
    std::map<batt::Token, std::unique_ptr<Connection>> connections_;
    std::map<std::string, batt::Token> current_;
    std::deque<DeferredClose> deferred_;
    std::size_t send_chunk_, max_clients_, peak_clients_ = 0;
    std::string boot_, command_line_;
    batt::Token next_token_ = 0;
    std::uint64_t total_bad_ = 0, replacements_ = 0, stale_events_ = 0, stale_results_ = 0, overloaded_ = 0;
    int stdin_flags_ = -1;
    bool cli_enabled_ = false, stopping_ = false, inputs_finished_ = false;
    bool quiet_ = false;
    batt::Time shutdown_at_{};
};
} // namespace

// 读取参数 -> 规范化库路径并加独占文件锁 -> 注册停止信号 -> 构造并运行服务。
// --test-* 是显式故障/期限测试入口；正常业务参数保留默认值即可。
int main(int argc, char** argv) {
    try {
        std::string host = "127.0.0.1"; unsigned short port = 19001;
        std::size_t chunk = 64 * 1024, max_clients = 128; batt::WorkerConfig config;
        batt::HttpOptions http_options;
        bool quiet = ::isatty(STDIN_FILENO) == 1;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "batt-access [--bind IPv4] [--port 19001] [--http-port 19002] [--max-clients 128] [--workers 2] [--db access.db] [--quiet|--verbose]\n"
                             "  Test hooks: --test-send-chunk N --test-freshness-ms N --test-query-timeout-ms N\n"
                             "              --test-queue-capacity N --test-slow-device DEV --test-slow-ms N\n"
                             "              --test-storage-fail OP | --test-storage-crash OP --test-storage-crash-phase PHASE\n"; return 0;
            }
            if (arg == "--quiet") { quiet = true; continue; }
            if (arg == "--verbose") { quiet = false; continue; }
            if (i + 1 >= argc) throw std::invalid_argument("missing option value");
            const std::string value = argv[++i];
            if (arg == "--bind") { host = value; continue; }
            if (arg == "--db") { config.storage.path = value; continue; }
            if (arg == "--test-storage-fail") { config.storage.fail_operation = value; continue; }
            if (arg == "--test-storage-crash") { config.storage.crash_operation = value; continue; }
            if (arg == "--test-storage-crash-phase") { config.storage.crash_phase = value; continue; }
            if (arg == "--test-slow-device") { config.slow_device = value; continue; }
            if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                throw std::invalid_argument("nonnegative integer option required");
            const auto n = std::stoul(value);
            if (arg == "--port") { if (n > 65535) throw std::invalid_argument("invalid port"); port = static_cast<unsigned short>(n); }
            else if (arg=="--http-port") { if (n>65535) throw std::invalid_argument("invalid HTTP port"); http_options.enabled=true; http_options.port=static_cast<unsigned short>(n); }
            else if (arg=="--test-api-timeout-ms") { if (n<50 || n>1000) throw std::invalid_argument("invalid API timeout"); http_options.application_wait=std::chrono::milliseconds(static_cast<std::int64_t>(n)); }
            else if (arg=="--test-api-capacity") { if (n<1 || n>16) throw std::invalid_argument("invalid API capacity"); http_options.command_capacity=n; }
            else if (arg=="--test-api-reply-delay-ms") { if (n>2000) throw std::invalid_argument("invalid HTTP reply delay"); http_options.reply_delay=std::chrono::milliseconds(static_cast<std::int64_t>(n)); }
            else if (arg == "--max-clients") { if (n == 0 || n > 4096) throw std::invalid_argument("invalid client limit"); max_clients = n; }
            else if (arg == "--workers") { if (n != 1 && n != 2) throw std::invalid_argument("workers must be 1 or 2"); config.workers = n; }
            else if (arg == "--test-send-chunk") { if (n == 0 || n > 65536) throw std::invalid_argument("invalid chunk"); chunk = n; }
            else if (arg == "--test-freshness-ms") { if (n == 0 || n > 90000) throw std::invalid_argument("invalid freshness"); config.freshness = std::chrono::milliseconds(static_cast<std::int64_t>(n)); }
            else if (arg == "--test-query-timeout-ms") { if (n < 100 || n > 3000) throw std::invalid_argument("invalid query timeout"); config.query_timeout = std::chrono::milliseconds(static_cast<std::int64_t>(n)); }
            else if (arg == "--test-queue-capacity") { if (n == 0 || n > 64) throw std::invalid_argument("invalid queue capacity"); config.queue_capacity = n; }
            else if (arg == "--test-slow-ms") { if (n > 1000) throw std::invalid_argument("invalid slow delay"); config.slow_delay = std::chrono::milliseconds(static_cast<std::int64_t>(n)); }
            else throw std::invalid_argument("unknown option: " + arg);
        }
        for (const auto& op : {config.storage.fail_operation, config.storage.crash_operation})
            if (!op.empty() && op != "login" && op != "login_replace" && op != "alarm" && op != "query_begin" && op != "query_finish" && op != "recovery")
                throw std::invalid_argument("invalid storage test operation");
        if (!config.storage.crash_operation.empty() && config.storage.crash_phase != "before_commit" &&
            config.storage.crash_phase != "after_commit" && config.storage.crash_phase != "mid_alarm" && config.storage.crash_phase != "mid_migration")
            throw std::invalid_argument("invalid storage crash phase");
        config.storage.path = batt::canonical_database(config.storage.path);
        config.storage.boot_id = boot_identity();
        const auto lock_path = config.storage.path + ".lock";
        batt::Fd database_lock(::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600));
        if (!database_lock || ::flock(database_lock.get(), LOCK_EX | LOCK_NB) < 0)
            throw batt::StorageError("database already in use, or lock file unavailable");
        struct sigaction action{}; action.sa_handler = request_stop; ::sigemptyset(&action.sa_mask);
        if (::sigaction(SIGINT, &action, nullptr) < 0 || ::sigaction(SIGTERM, &action, nullptr) < 0)
            throw batt::sys_error("sigaction");
        Server server(host, port, chunk, max_clients, config, quiet, http_options); server.run(); return 0;
    } catch (const std::exception& error) { std::cerr << "batt-access: " << error.what() << '\n'; return 1; }
}
