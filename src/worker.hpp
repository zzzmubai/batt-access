/*
 * 文件作用：声明固定设备分片 worker 及 I/O 与业务线程间的消息契约。
 * Work 是输入：协议帧、断开通知或 CLI 命令；WorkerOutput 是结果、日志与连接操作请求。
 * 相同 devId 通过固定哈希进入同一 worker，在该线程内顺序处理业务。
 * Worker 独占 Sessions、Queries 和 Store；Workers 管理线程集合及公共返回队列。
 * 本项目默认两个分片，也支持一个分片作对照，没有运行时扩缩容或通用线程池。
 * 实现见 worker.cpp；输入由 access.cpp 投递，输出仍由 access.cpp 操作 socket。
 */
#pragma once
#include "bounded_queue.hpp"
#include "query_task.hpp"
#include "store.hpp"
#include "api.hpp"
#include <future>
#include <memory>
#include <thread>

namespace batt {
// FNV-1a 哈希后取模，保证相同设备在当前 worker 数量下始终落到同一个分片。
inline std::size_t device_shard(const std::string& name, std::size_t workers) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto ch : name) { hash ^= static_cast<unsigned char>(ch); hash *= 1099511628211ULL; }
    return static_cast<std::size_t>(hash % workers);
}
enum class WorkKind { frame, disconnected, command };
// 消息按值携带数据；enqueued 在对象创建时记录，主要用于排队耗时日志，并非查询起点。
struct Work {
    WorkKind kind = WorkKind::frame;
    Token token = 0;
    std::string device;
    Json frame;
    std::string command;
    std::string reason;
    std::int64_t task_id = 0;
    Time enqueued = Clock::now();
    Time login_deadline{};
    bool initial_login = false;
    std::optional<Command> action;
    std::shared_ptr<ApiContext> api;
};
// worker 不直接碰 fd；用 bind/replaced/close_reason 请求 I/O 更新连接，用 message 请求发送。
struct WorkerOutput {
    std::size_t worker = 0;
    std::size_t registered = 0;
    Token token = 0;
    std::optional<Json> message;
    std::optional<Time> deadline;
    std::optional<std::string> bind;
    std::optional<Token> replaced;
    std::optional<Json> snapshot;
    std::vector<Json> logs;
    std::string close_reason;
    // 标记一个已受理协议帧已处理完，I/O 据此减少连接上的 inflight 计数。
    bool frame_done = false;
};
// 默认参数与显式测试钩子集中于此；slow_device/slow_delay 用于人为慢业务对照。
struct WorkerConfig {
    std::size_t workers = 2;
    std::size_t queue_capacity = 64;
    std::chrono::milliseconds freshness = std::chrono::seconds(90);
    std::chrono::milliseconds query_timeout = std::chrono::seconds(3);
    std::string slow_device;
    std::chrono::milliseconds slow_delay{0};
    StoreOptions storage;
};

class Worker {
public:
    Worker(std::size_t index, const WorkerConfig& config, BoundedQueue<WorkerOutput>& output,
           std::atomic<std::int64_t>& next_task, StoreShared& storage_shared);
    ~Worker();
    // I/O 只尝试入队，不能等待容量；finish 允许排空，abort 用于失败时取消。
    bool submit(Work work) { return input_.try_push(std::move(work)); }
    void finish() { finishing_.store(true); input_.close(); }
    void abort();
    bool done() const { return done_.load(); }
    bool failed() const { return failed_.load(); }
    std::size_t queued() const { return input_.size(); }
    std::size_t peak() const { return input_.peak(); }
    void wait_ready() { ready_.get_future().get(); }
    std::size_t pending() const { return pending_count_.load(); }
    bool processing() const { return processing_.load(); }
private:
    void run();
    void process(const Work& work);
    void command(const Work& work);
    void emit(WorkerOutput output);
    void logs(std::vector<Json> events, Token token = 0);
    void one(Json event, Token token = 0);
    std::size_t index_;
    WorkerConfig config_;
    BoundedQueue<Work> input_;
    BoundedQueue<WorkerOutput>& output_;
    // 以下业务对象只由 thread_ 使用；跨线程读取的统计和停止标志使用 atomic。
    Sessions sessions_;
    Queries queries_;
    StoreShared& storage_shared_;
    std::unique_ptr<Store> store_;
    std::promise<void> ready_;
    std::atomic<std::size_t> pending_count_{0};
    std::atomic<bool> processing_{false};
    std::atomic<bool> aborted_{false}, done_{false}, failed_{false}, finishing_{false};
    std::mutex delay_mutex_;
    std::condition_variable delay_wake_;
    // 声明在依赖成员之后，构造启动线程时前面的状态已初始化；析构必须先 join。
    std::thread thread_;
};

class Workers {
public:
    explicit Workers(const WorkerConfig& config);
    ~Workers();
    bool submit(std::size_t shard, Work work) { return workers_.at(shard)->submit(std::move(work)); }
    std::optional<WorkerOutput> take() { return output_.try_pop(); }
    void finish();
    void abort();
    bool done() const;
    bool failed() const;
    bool output_empty() const { return output_.size() == 0; }
    std::size_t count() const { return workers_.size(); }
    Json queues() const;
private:
    // Keep shared storage alive until all workers have joined.
    // 所有 worker 共用有界返回队列；共享存储先于 workers_ 声明，析构时在线程之后释放。
    BoundedQueue<WorkerOutput> output_{128};
    std::atomic<std::int64_t> next_task_{1};
    StoreShared storage_shared_;
    std::vector<std::unique_ptr<Worker>> workers_;
};
} // namespace batt
