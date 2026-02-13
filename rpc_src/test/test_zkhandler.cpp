#include <iostream>
#include <nlohmann/json.hpp>

#include "zk_handler.h"

int main() {
  try {
    rpc::ServiceConfig service_config;
    if (!service_config.Init("../config/service_config.json")) {
      std::cerr << "service_config init failed" << std::endl;
      return -1;
    }
    nlohmann::json zk_config;
    zk_config["zk_host"] = "localhost";
    zk_config["zk_port"] = 2181;
    zk_config["zk_namespace"] = "mt_rpc";
    zk_config["retry_interval"] = 3;

    auto& zk_handler = rpc::ZkHandler::GetInstance();
    if (!zk_handler.InitZkHandler(zk_config)) {
      std::cerr << "zk_handler init failed" << std::endl;
      return -1;
    }

    if (!zk_handler.RegistryAllNode(&service_config)) {
      std::cerr << "zk_handler registry failed" << std::endl;
      return -1;
    }

    std::cout << "test_zkhandler success" << std::endl;
    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  } catch (const std::exception& e) {
    std::cerr << "test_zkhandler failed: " << std::endl;
    return -1;
  }
  return 0;
}