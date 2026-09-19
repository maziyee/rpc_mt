#include "zk_config.h"

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "log_manager.h"

bool rpc::ZkConfig::InitZkConfig(const std::string &config_path) {
  std::ifstream zk_file(config_path);
  if (!zk_file.is_open()) {
    return false;
  }
  nlohmann::json zk_config = nlohmann::json::parse(zk_file);
  zk_file.close();
  this->SetHost(zk_config.value("zk_host", "localhost"));
  this->SetPort(zk_config.value("zk_port", 2181));
  this->SetNamespace(zk_config.value("zk_namespace", "/rpc_mt/you_dian"));
  this->SetRetryTime(zk_config.value("zk_retry_times", 3));
  return true;
}
