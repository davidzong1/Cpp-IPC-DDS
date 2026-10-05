#pragma once
#include "dzIPC/net/shared_config.h"
#include <memory>
#include <string>

namespace dzIPC::net
{
class GatewayRuntime
{
  public:
    explicit GatewayRuntime(GatewayConfig config);
    ~GatewayRuntime();
    GatewayRuntime(const GatewayRuntime &) = delete;
    GatewayRuntime &operator=(const GatewayRuntime &) = delete;
    void stop();
    bool running() const noexcept;
    std::string status_json() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace dzIPC::net
