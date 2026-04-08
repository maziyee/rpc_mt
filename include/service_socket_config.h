#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "log_manager.h"

namespace rpc {
class ServiceSocketConfig {
 public:
  bool Init(const std::string& config_path);

  std::string GetServiceIp() const { return service_ip_; }
  uint16_t GetServicePort() const { return service_port_; }
  uint16_t GetServiceMaxConnections() const { return service_max_connections_; }
  uint16_t GetServiceThreadTimeout() const { return service_thread_timeout_; }
  std::string GetServiceNamePrefix() const { return service_name_prefix_; }

 private:
  void SetServiceIp(const std::string& service_ip) { service_ip_ = service_ip; }
  void SetServicePort(uint16_t service_port) { service_port_ = service_port; }
  void SetServiceMaxConnections(uint16_t service_max_connections) {
    service_max_connections_ = service_max_connections;
  }
  void SetServiceThreadTimeout(uint16_t service_thread_timeout) {
    service_thread_timeout_ = service_thread_timeout;
  }
  void SetServiceNamePrefix(const std::string& service_name_prefix) {
    service_name_prefix_ = service_name_prefix;
  }

 private:
  std::string service_ip_;
  uint16_t service_port_;
  uint16_t service_max_connections_;
  uint16_t service_thread_timeout_;
  std::string service_name_prefix_;
};
}  // namespace rpc