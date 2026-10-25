/*
 * 文件作用：实现线程安全的有界 FIFO 队列，用于 I/O 与 worker 之间传递消息。
 * worker.hpp 为每个 worker 建立输入队列，并为所有 worker 建立公共返回队列。
 * 输入端使用 try_push，I/O 不等待空位；返回端可用 push_wait 等待消费。
 * 互斥锁保护队列状态，条件变量等待可读/可写条件，容量限制避免无限积压。
 * close 停止新入队但允许取完已有数据，供正常退出时排空使用。
 */
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace batt {
template<class T> class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0) throw std::invalid_argument("zero queue capacity");
    }
    // 非阻塞入队：满或关闭时立即返回 false，由调用方决定拒绝命令或关闭过载连接。
    bool try_push(T item) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (closed_ || items_.size() >= capacity_) return false;
        items_.push_back(std::move(item));
        if (items_.size() > peak_) peak_ = items_.size();
        readable_.notify_one();
        return true;
    }
    // 返回队列满时等待；谓词处理虚假唤醒，并在关闭或取消时结束等待。
    bool push_wait(T item, const std::atomic<bool>& abort) {
        std::unique_lock<std::mutex> guard(mutex_);
        writable_.wait(guard, [&] { return closed_ || abort.load() || items_.size() < capacity_; });// 等待队列满或关闭或取消->谓词 = true：满足退出等待的条件 → 醒来干活
        if (closed_ || abort.load()) return false;
        items_.push_back(std::move(item));
        if (items_.size() > peak_) peak_ = items_.size();
        readable_.notify_one();
        return true;
    }
    // 等待数据至多 timeout；超时或关闭后无数据都返回空，worker 随后可检查业务期限。
    std::optional<T> pop_for(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> guard(mutex_);
        readable_.wait_for(guard, timeout, [&] { return closed_ || !items_.empty(); });
        if (items_.empty()) return std::nullopt;
        T item = std::move(items_.front());
        items_.pop_front();
        writable_.notify_one();
        return item;
    }
    std::optional<T> try_pop() { return pop_for(std::chrono::milliseconds(0)); }
    // 同时唤醒生产者与消费者，避免退出时有人永远睡在条件变量上。
    void close() {
        std::lock_guard<std::mutex> guard(mutex_);
        closed_ = true;
        readable_.notify_all();
        writable_.notify_all();
    }
    // 只有“已关闭且已取空”才表示排空；运行中队列暂时为空不代表 worker 应退出。
    bool drained() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return closed_ && items_.empty();
    }
    std::size_t size() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return items_.size();
    }
    std::size_t peak() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return peak_;
    }
private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable readable_, writable_;
    std::deque<T> items_;
    bool closed_ = false;
    std::size_t peak_ = 0;
};
} // namespace batt
