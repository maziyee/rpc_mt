#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "log_manager.h"

namespace rpc {

struct RegistryNodeInfo {
  std::string address;
  int port;
};
class ServiceConfig {
 public:
  ServiceConfig() = default;
  ~ServiceConfig() = default;
  bool Init(const std::string& config_path);
  const std::string& GetServiceName() const { return this->service_name_; }
  const std::string& GetVersion() const { return this->version_; }
  const std::vector<RegistryNodeInfo>& GetRegistryNodes() const {
    return this->registry_nodes_;
  }
  void SetServiceName(const std::string& service_name) {
    this->service_name_ = service_name;
  }
  void SetVersion(const std::string& version) { this->version_ = version; }

  size_t GetRegistryNodeSize() const { return this->registry_nodes_.size(); }

  ServiceConfig& operator=(const ServiceConfig& other) = delete;
  ServiceConfig(const ServiceConfig& other) = delete;
  ServiceConfig& operator=(ServiceConfig&& other) = delete;
  ServiceConfig(ServiceConfig&& other) = delete;

 private:
  std::string service_name_;
  std::string version_;
  std::vector<RegistryNodeInfo> registry_nodes_;
};
}  // namespace rpc
