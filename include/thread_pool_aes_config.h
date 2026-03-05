#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "log_manager.h"
#include "thread_pool.h"

namespace rpc {
class ThreadAesConfig {
 public:
  ThreadAesConfig() = default;
  std::string &GetMasterKey() { return master_key; }
  meeting_ctrl::ThreadStruct &GetThreadConfig() { return thread_config; }

  void SetMasterKey(const std::string &key) { master_key = key; }
  bool InitThreadAes(const std::string &config_file);

 private:
 private:
  std::string master_key;
  meeting_ctrl::ThreadStruct thread_config;
};
}  // namespace rpc
