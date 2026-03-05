#include "thread_pool_aes_config.h"

#include <fstream>

#include "log_manager.h"
#include "thread_pool.h"

bool rpc::ThreadAesConfig::InitThreadAes(const std::string& config_file) {
  try {
    std::ifstream thread_aes_config(config_file);
    if (!thread_aes_config.is_open()) {
      LOG_ERROR("the config file is empty");
      return false;
    }
    nlohmann::json thread_aes_load = nlohmann::json::parse(thread_aes_config);
    if (thread_aes_load.contains("Aes")) {
      this->SetMasterKey(
          thread_aes_load["Aes"].value("masterkey", "rpc_mt_aes_encrypt_key"));
    } else {
      LOG_ERROR("Init Aes failed ");
      return false;
    }
    if (thread_aes_load.contains("Thread_pool")) {
      this->thread_config = std::move(meeting_ctrl::ThreadStruct(
          thread_aes_load["Thread_pool"].value("core_threads", 8),
          thread_aes_load["Thread_pool"].value("max_threads", 10),
          thread_aes_load["Thread_pool"].value("keep_alive_time", 60),
          thread_aes_load["Thread_pool"].value("queue_size", 10000)));
    } else {
      LOG_ERROR("the thread_pool init failed");
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    LOG_ERROR("Init Threadpool and Aes failed : {}", e.what());
    return false;
  }
}
