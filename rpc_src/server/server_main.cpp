#include <iostream>

#include "aes_encrypt.h"
#include "log_manager.h"
#include "rpc_config_mananger.h"
#include "service_registry.h"
#include "thread_pool.h"
#include "thread_single.h"

int main() {
  if (!rpc::RpcConfigManager::GetInstance().Init(
          "../config/spdlog_config.json", "../config/service_config.json",
          "../config/thread_pool_aes_config.json")) {
    std::cerr << "RpcConfigManager init error" << std::endl;
    return -1;
  };
  LOG_INFO("RpcConfigManager init success");
  try {
    LOG_INFO("Test ThreadPool");
    meeting_ctrl::ThreadSingle::Init(rpc::RpcConfigManager::GetInstance()
                                         .GetThreadAesConfig()
                                         ->GetThreadConfig());
    auto res = meeting_ctrl::ThreadSingle::GetInstance().Enqueue(
        meeting_ctrl::TaskPriority::kHIGH, []() {
          LOG_INFO("TestBasicTask");
          std::this_thread::sleep_for(std::chrono::seconds(10));
          return 42;
        });
    int result = res.get();
    LOG_INFO("TestBasicTask result: {}", result);
  } catch (std::exception& e) {
    std::cerr << "ThreadPool init error" << e.what() << std::endl;
  }
  try {
    LOG_INFO("Test Aes module");
    rpc::AesEncrypt::GetInstance().Init(rpc::RpcConfigManager::GetInstance()
                                            .GetThreadAesConfig()
                                            ->GetMasterKey());
    std::string plain_text = "hello world";
    std::string cipher_text, decrypt_text;
    rpc::AesEncrypt::GetInstance().Encrypt(plain_text, cipher_text);
    rpc::AesEncrypt::GetInstance().Decrypt(cipher_text, decrypt_text);
    if (plain_text == decrypt_text) {
      LOG_INFO("Test Aes module success");
    }
    LOG_INFO(
        "Test Aes module success, plain_text: {}, cipher_text: {}, "
        "decrypt_text: {}",
        plain_text, cipher_text, decrypt_text);
  } catch (std::exception& e) {
    std::cerr << "Aes module init error" << e.what() << std::endl;
  }
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
    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(1));

      if (!service_registry.IsConnected()) {
        LOG_ERROR("the connect lost");
        break;
      }

      std::string input;
      std::cin >> input;
      if (input == "quit") {
        break;
      }
    }
  } catch (std::exception& e) {
    std::cerr << "service_registry failed" << e.what() << std::endl;
  }
  return 0;
}