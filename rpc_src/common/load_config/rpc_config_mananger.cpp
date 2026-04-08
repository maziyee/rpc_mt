#include "rpc_config_mananger.h"

#include <fstream>
#include <iostream>

#include "log_manager.h"

bool rpc::RpcConfigManager::Init(const std::string& log_config_path,
                                 const std::string& service_config_path,
                                 const std::string& thread_pool_aes_path,
                                 const std::string& service_socket_config_path,
                                 const std::string& zk_config_path) {
  try {
    if (!this->spdlog_config_->InitSpdlog(log_config_path)) {
      std::cerr << "InitSpdlog config error" << std::endl;
      return false;
    }
    if (!Logger::GetInstance().Init(this->GetSpdlogConfig())) {
      std::cerr << "Init Logger error" << std::endl;
      return false;
    }
    LOG_INFO("Init spdlog config success with level: {}",
             this->GetSpdlogConfig()->GetLevel());

    if (!this->service_config_->Init(service_config_path)) {
      LOG_ERROR("Init service config error");
      return false;
    }
    if (!this->thread_aes_config_->InitThreadAes(thread_pool_aes_path)) {
      LOG_ERROR("Init threadpool and aes module failed");
      return false;
    }
    if (!this->service_socket_config_->Init(service_socket_config_path)) {
      LOG_ERROR("Init service socket config error");
      return false;
    }
    if (!this->zk_config_->InitZkConfig(zk_config_path)) {
      LOG_ERROR("Init zk config error");
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    std::cerr << "Init RpcConfigManager error: " << e.what() << std::endl;
    return false;
  }
}
