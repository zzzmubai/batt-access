/*
 * 文件作用：缓存服务端待发字节，并在 socket 可写时继续发送。
 * 调用关系：access.cpp::drain_outputs -> enqueue；send_pending -> flush。
 * 每条连接拥有自己的 SendBuffer，由 I/O 线程独占，不在内部加锁。
 * 重点处理短写、EINTR、EAGAIN，以及带 deadline 的 910 查询。
 * flush 返回发送进度，连接是否关闭由上层决定；默认待发字节上限 256 KiB。
 */
#pragma once
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <stdexcept>
#include <string>
#include <optional>

namespace batt {
// drained=已排空；pending=仍有待发；disconnected=发送失败；deadline_partial=半帧查询过期。
enum class Flush { drained, pending, disconnected, deadline_partial };
class SendBuffer {
/*  
 `batt::SendBuffer`：**非阻塞 socket 发送缓冲区（发包队列）**，配合 epoll LT/ET 使用。

- `enqueue()`：把要发的数据块（chunk）排入发送队列；每个块可以附带一个超时截止时间 deadline
- `flush()`：**尝试调用底层 sender（就是 write/send）往外写数据**；循环写、处理分片、EAGAIN、EINTR、块超时，返回枚举 `Flush` 告知上层这次 flush 的结果
- 和前面 `JsonSlice` 是搭档：JsonSlice 切出完整 JSON → `send_buf.enqueue(json_str)` → epoll 可写事件触发 `flush()`
          
*/
public:
    explicit SendBuffer(std::size_t limit = 256 * 1024) : limit_(limit) {}
    // 只排队，不调用 send；queued_ 统计尚未发出的字节，超限交给上层关闭连接。
    void enqueue(std::string bytes, std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt) {
        if (bytes.size() > limit_ - queued_) throw std::runtime_error("send_overflow");
        if (bytes.empty()) return;
        queued_ += bytes.size();
        chunks_.push_back({std::move(bytes), deadline});
    }
    bool empty() const { return chunks_.empty(); }
    std::size_t size() const { return queued_; }

    // sender 通过回调注入，便于单测模拟短写和暂时不可写；now 同样可替换以测试期限。
    // max_chunk 限制单次写入，budget 限制本轮总写入量，避免一条连接长期占用 I/O 线程。
    Flush flush(const std::function<std::ptrdiff_t(const char*, std::size_t)>& sender,
                std::size_t max_chunk = 64 * 1024,
                std::size_t budget = 64 * 1024,
                const std::function<std::chrono::steady_clock::time_point()>& now =
                    [] { return std::chrono::steady_clock::now(); }) {
        if (max_chunk == 0) throw std::invalid_argument("zero_send_chunk");
        while (!chunks_.empty() && budget > 0) {
            const auto& chunk = chunks_.front();
            const auto& first = chunk.bytes;
            // 过期且一个字节都未发送时可以整块丢弃；已发出半帧则必须通知上层关闭连接。
            // 若半帧之后继续拼下一对象，对端会收到损坏的 JSON 字节流。
            if (chunk.deadline && now() >= *chunk.deadline) {
                if (offset_ != 0) return Flush::deadline_partial;
                queued_ -= first.size();
                chunks_.pop_front();
                continue;
            }
            const auto count = std::min({first.size() - offset_, max_chunk, budget});
            const auto n = sender(first.data() + offset_, count);
            if (n < 0) {
                if (errno == EINTR) continue;
                // 暂时不可写不等于断线：保留队首块和 offset_，等待下次可写时继续。
                if (errno == EAGAIN || errno == EWOULDBLOCK) return Flush::pending;
                return Flush::disconnected;
            }
            if (n == 0) return Flush::disconnected;
            const auto written = static_cast<std::size_t>(n);
            if (written > count) throw std::logic_error("invalid_sender_result");
            offset_ += written;
            queued_ -= written;
            budget -= written;
            if (offset_ == first.size()) {
                chunks_.pop_front();
                offset_ = 0;
            }
        }
        return chunks_.empty() ? Flush::drained : Flush::pending;
    }
private:
    std::size_t limit_;
    std::size_t queued_ = 0;
    // 只记录队首块已经发出的长度；块完全发送后归零，下一块从头开始。
    std::size_t offset_ = 0;
    struct Chunk {
        std::string bytes;
        std::optional<std::chrono::steady_clock::time_point> deadline;
    };
    std::deque<Chunk> chunks_;
};
} // namespace batt
