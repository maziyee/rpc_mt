#pragma once

#include <string>

#include "log_manager.h"
#include "message.pb.h"
namespace rpc {
class ProtobufSerializer {
 public:
  template <typename T>
  static std::string Serialize(const T& message) {
    try {
      return message.SerializeAsString();
    } catch (std::exception& e) {
      LOG_ERROR("ProtobufSerializer Serialize error: {}", e.what());
      return "";
    }
  }
  template <typename T>
  static bool Deserialize(const std::string& data, T& message) {
    try {
      T tmp;
      if (!tmp.ParseFromString(data)) {
        LOG_ERROR("ProtobufSerializer Deserialize error: {}",
                  tmp.InitializationErrorString());
        return false;
      }
      message = tmp;
      return true;
    } catch (std::exception& e) {
      LOG_ERROR("ProtobufSerializer Deserialize error: {}", e.what());
      return false;
    }
  }
};
}  // namespace rpc