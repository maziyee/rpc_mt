#pragma once

#include <nlohmann/json.hpp>
#include <string>

#include "log_manager.h"

namespace rpc {
class JsonSerializer {
 public:
  template <typename T>
  static std::string Serialize(const T& t) {
    try {
      return nlohmann::json(t).dump();
    } catch (const std::exception& e) {
      LOG_ERROR("Serialize error: {}", e.what());
      return "";
    }
  }
  template <typename T>
  static bool Deserialize(const std::string& str, T& t) {
    try {
      nlohmann::json json = nlohmann::json::parse(str);
      t = json.get<T>();
      return true;
    } catch (const std::exception& e) {
      LOG_ERROR("Deserialize error: {}", e.what());
      return false;
    }
  }
};
}  // namespace rpc