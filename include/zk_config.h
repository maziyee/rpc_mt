#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "log_manager.h"

namespace rpc {
class ZkConfig {
 public:
  bool InitZkConfig(const std::string& config_path);
  std::string GetHost() const { return zk_host_; }
  uint16_t GetPort() const { return zk_port_; }
  std::string GetNamespace() const { return zk_namespace_; }
  uint16_t GetRetryInterval() const { return retry_time_; }
  bool Empty() { return zk_host_.empty() && zk_namespace_.empty(); }

 private:
  void SetHost(const std::string& host) { zk_host_ = host; }
  void SetPort(uint16_t port) { zk_port_ = port; }
  void SetNamespace(const std::string& ns) { zk_namespace_ = ns; }
  void SetRetryTime(uint16_t time) { retry_time_ = time; }

 private:
  std::string zk_host_;
  uint16_t zk_port_;
  std::string zk_namespace_;
  uint16_t retry_time_;
};
}  // namespace rpc
