#pragma once

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "service_config.h"
#include "spdlog_config.h"

namespace rpc {
class RpcConfigManager {
 public:
  static RpcConfigManager& GetInstance() {
    static RpcConfigManager instance;
    return instance;
  }
  ~RpcConfigManager() = default;
  bool Init(const std::string& log_config_path,
            const std::string& service_config_path);

  SpdlogConfig* GetSpdlogConfig() const { return spdlog_config_.get(); }

  ServiceConfig* GetServiceConfig() const { return service_config_.get(); }

 private:
  RpcConfigManager()
      : spdlog_config_(std::make_unique<SpdlogConfig>()),
        service_config_(std::make_unique<ServiceConfig>()){};

 private:
  std::unique_ptr<SpdlogConfig> spdlog_config_;
  std::unique_ptr<ServiceConfig> service_config_;
};
}  // namespace rpc