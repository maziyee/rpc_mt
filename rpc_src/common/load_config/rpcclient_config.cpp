#include "rpcclient_config.h"

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

#include "aes_encrypt.h"
#include "log_manager.h"

bool rpc::RpcClientConfig::Init(const std::string& config_path) {
  try {
    std::ifstream config_file(config_path);
    if (!config_file.is_open()) {
      return false;
    }
    nlohmann::json config = nlohmann::json::parse(config_file);
    config_file.close();
    this->SetRetryTimes(config.value("retry_times", 3));
    this->SetTimeoutMs(config.value("timeout_ms", 1000));
    this->SetClientIp(config.value("client_ip", "127.0.0.1"));
    this->SetLoadBalance(config.value("load_balance", "random"));
    std::string master_key = config.value("master_key", "rpc_mt_aes_encrypt_key");
    rpc::AesEncrypt::GetInstance().Init(master_key);
    return true;
  } catch (const std::exception& e) {
    LOG_ERROR("Init RpcClientConfig error: {}", e.what());
    return false;
  }
}
