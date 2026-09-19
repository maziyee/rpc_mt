#include "service_socket_config.h"

#include <fstream>
#include <nlohmann/json.hpp>

#include "log_manager.h"

bool rpc::ServiceSocketConfig::Init(const std::string& config_path) {
  try {
    std::ifstream config_file(config_path);
    if (!config_file.is_open()) {
      LOG_ERROR("the service file open failed");
      return false;
    }
    nlohmann::json service_config = nlohmann::json::parse(config_file);
    this->SetServiceIp(service_config.value("servers_ip", "0.0.0.0"));
    this->SetServicePort(service_config.value("servers_port", 8989));
    this->SetServiceMaxConnections(
        service_config.value("max_connections", 800000));
    this->SetServiceThreadTimeout(service_config.value("timeout", 5000));
    this->SetServiceNamePrefix(
        service_config.value("servers_name_prefix", "rpc_mt"));
    return true;
  } catch (const std::exception& e) {
    LOG_ERROR("load service socket config failed {} ", e.what());
    return false;
  }
}
