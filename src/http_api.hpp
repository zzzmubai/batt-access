#pragma once
#include "api.hpp"
#include <memory>
namespace batt {
struct HttpOptions {
    bool enabled = false;
    unsigned short port = 19002;
    std::chrono::milliseconds reply_delay{0}; // explicit dropped-response test hook
    std::chrono::milliseconds application_wait{1000};
    std::size_t command_capacity=16;
};
class HttpGateway {
public:
    HttpGateway(ApiBridge& bridge, const HttpOptions& options);
    ~HttpGateway();
    void stop();
    unsigned port() const;
    Json stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
