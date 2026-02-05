#include <iostream>

#include "log_manager.h"
#include "rpc_config_mananger.h"
#include "service_registry.h"

int main() {
  if (!rpc::RpcConfigManager::GetInstance().Init(
          "../config/spdlog_config.json")) {
    std::cerr << "RpcConfigManager init error" << std::endl;
    return -1;
  };
  LOG_INFO("RpcConfigManager init success");
  try {
    ServiceRegistry service_registry("127.0.0.1:2181");
    if (!service_registry.IsConnected()) {
      std::cerr << "Zookeepr connected failed " << std::endl;
      return -1;
    }
    const std::string& service_name = "User_service";
    const std::string& service_addr = "127.0.0.1:8080";
    if (!service_registry.Register(service_name, service_addr)) {
      std::cerr << "registry failed" << std::endl;
      return -1;
    }
  } catch (std::exception& e) {
    std::cerr << "service_registry failed" << e.what() << std::endl;
  }
  return 0;
}