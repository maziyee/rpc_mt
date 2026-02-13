#include "service_config.h"

#include <fstream>

#include "log_manager.h"

bool rpc::ServiceConfig::Init(const std::string& config_path) {
  std::ifstream config_file(config_path);
  try {
    if (!config_file.is_open()) {
      LOG_ERROR("the service file open failed");
      return false;
    }
    nlohmann::json service_config = nlohmann::json::parse(config_file);
    this->SetVersion(service_config.value("version", "1.0.0"));
    this->SetServiceName(service_config.value("service_name", "User_service"));

    if (service_config.contains("registry_nodes") &&
        service_config["registry_nodes"].is_array()) {
      for (const auto node : service_config["registry_nodes"]) {
        RegistryNodeInfo info;
        info.address = node.value("address", "");
        info.port = node.value("port", 0);
        if (info.address.empty() || info.port == 0) {
          LOG_WARN("load service config failed service_address : {} , port: {}",
                   info.address, info.port);
          continue;
        }
        this->registry_nodes_.push_back(info);
      }
    }
    LOG_INFO("service load {}", this->GetRegistryNodeSize());
    return true;
  } catch (const std::exception& e) {
    LOG_ERROR("load service config failed {} ", e.what());
    return false;
  }
}