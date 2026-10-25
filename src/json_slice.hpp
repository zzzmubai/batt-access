/*
 * 文件作用：将 TCP 字节流切成独立、完整的 JSON 对象，供收包端复用。
 * 调用关系：access.cpp::receive / sim.cpp::receive_available -> feed -> 完整对象回调。
 * TCP 的一次 recv 可能得到半个对象或多个对象，因此解析状态必须跨调用保留。
 * 这里只识别括号和字符串边界；JSON 语法由 protocol.hpp 的 parse_object 校验。
 * 默认对象上限 64 KiB、嵌套上限 64 层；残帧是否超时由上层定期检查。
 */
#pragma once
#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace batt {
// 单调时钟用于运行中的期限和数据年龄，避免系统时间校准影响超时判断。
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
class StreamError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// This layer finds object boundaries; the JSON library validates their grammar.
class JsonSlice {
public:
    explicit JsonSlice(std::size_t limit = 64 * 1024, std::size_t depth = 64)
        : limit_(limit), depth_limit_(depth) {}

    // 逐字节推进状态机；complete 可能在一次 feed 中被调用多次，也可能一次都不调用。
    void feed(std::string_view bytes, Time now,
              const std::function<void(std::string&&)>& complete) {
        for (const char ch : bytes) {
            if (buffer_.empty()) {
                if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
                if (ch != '{') throw StreamError("invalid_prefix");
                started_ = now;
            }
            buffer_.push_back(ch);
            if (buffer_.size() > limit_) throw StreamError("object_too_large");
            // 字符串里的括号没有结构含义；转义状态也要跨 recv 保留，才能正确识别引号。
            if (in_string_) {
                if (escaped_) escaped_ = false;
                else if (ch == '\\') escaped_ = true;
                else if (ch == '"') in_string_ = false;
                continue;
            }
            if (ch == '"') in_string_ = true;
            else if (ch == '{' || ch == '[') {
                stack_.push_back(ch);
                if (stack_.size() > depth_limit_) throw StreamError("too_deep");
            } else if (ch == '}' || ch == ']') {
                const char expected = ch == '}' ? '{' : '[';
                if (stack_.empty() || stack_.back() != expected)
                    throw StreamError("mismatched_boundary");
                stack_.pop_back();
                // 最外层对象闭合后交换出缓冲；下一字节即可开始下一个连续 JSON 对象。
                if (stack_.empty()) {
                    std::string object;
                    object.swap(buffer_);
                    complete(std::move(object));
                }
            }
        }
    }

    bool incomplete() const { return !buffer_.empty(); }
    std::size_t buffered() const { return buffer_.size(); }
    // 从对象首字节计时；持续发送零碎字节不会无限延长这一帧的期限。
    bool expired(Time now, std::chrono::milliseconds timeout) const {
        return incomplete() && now - started_ >= timeout;
    }

private:
    std::size_t limit_;
    std::size_t depth_limit_;
    std::string buffer_;
    std::vector<char> stack_;
    bool in_string_ = false;
    bool escaped_ = false;
    Time started_{};
};
} // namespace batt
