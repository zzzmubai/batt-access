/*
 * 文件作用：提供 Linux 网络基础工具，供服务端 access.cpp 和模拟终端 sim.cpp 使用。
 * Fd 用 RAII 管理文件描述符：离开作用域自动 close，移动时转移所有权。
 * address() 构造 IPv4 地址，sys_error() 将 errno 转成 C++ 异常。
 * 阅读重点：fd 是操作系统资源编号；业务会话身份由 session.hpp 中的 Token 表示。
 * 本文件只封装资源与地址，不负责连接建立、收发循环和业务协议。
 */
#pragma once
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace batt {
// 一个 Fd 独占一个描述符。禁止拷贝，避免两个对象析构时重复关闭同一个 fd。
class Fd {
public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    // 移动后把源对象置为 -1，使源对象析构时不再关闭已经转交的资源。
    Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) { reset(); fd_ = other.fd_; other.fd_ = -1; }
        return *this;
    }
    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }
    void reset() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
private:
    int fd_;
};
inline std::runtime_error sys_error(const std::string& action) {
    return std::runtime_error(action + ": " + std::strerror(errno));
}
// 只接收数字形式的 IPv4；htons 把主机端口转换成网络字节序，不进行 DNS 查询。
inline sockaddr_in address(const std::string& host, unsigned short port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
        throw std::invalid_argument("numeric IPv4 address required");
    return addr;
}
} // namespace batt

//batt::Fd fd(::socket(AF_INET, SOCK_STREAM, 0)); // 自动关门管家上线
//auto addr = batt::address("0.0.0.0", 8080);      // 一键生成地址