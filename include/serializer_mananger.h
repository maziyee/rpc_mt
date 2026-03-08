#pragma once
#include <string>

#include "json_serializer.h"
#include "log_manager.h"
#include "protobuf_serializer.h"

namespace rpc {
enum SerializerType {
  JSON,
  PROTOBUF,
};

class SerializerManager {
 public:
  template <typename T>
  static std::string Serialize(const T& msg, SerializerType type) {
    try {
      switch (type) {
        case SerializerType::JSON:
          if constexpr (std::is_same_v<T, nlohmann::json>) {
            return Serialize_Json(msg);
          } else {
            LOG_ERROR("SerializerManager::Serialize error: {}",
                      "T is not nlohmann::json");
            return "";
          }
        case SerializerType::PROTOBUF:
          if constexpr (std::is_base_of_v<google::protobuf::Message, T>) {
            return Serialize_Protobuf(msg);
          } else {
            LOG_ERROR("SerializerManager::Serialize error: {}",
                      "T is not google::protobuf::Message");
            return "";
          }
        default:
          LOG_ERROR("SerializerManager::Serialize error: {}", "Unknown type");
          return "";
      }
    } catch (std::exception& e) {
      LOG_ERROR("SerializerManager::Serialize error: {}", e.what());
      return "";
    }
  }

  template <typename T>
  static bool Deserialize(const std::string& msg, T& out, SerializerType type) {
    try {
      switch (type) {
        case SerializerType::JSON:
          if constexpr (std::is_same_v<T, nlohmann::json>) {
            return Deserialize_Json(msg, out);
          } else {
            LOG_ERROR("SerializerManager::Deserialize error: {}",
                      "T is not nlohmann::json");
            return false;
          }
        case SerializerType::PROTOBUF:
          if constexpr (std::is_base_of_v<google::protobuf::Message, T>) {
            return Deserialize_Protobuf(msg, out);
          } else {
            LOG_ERROR("SerializerManager::Deserialize error: {}",
                      "T is not google::protobuf::Message");
            return false;
          }
        default:
          LOG_ERROR("SerializerManager::Deserialize error: {}", "Unknown type");
          return false;
      }
    } catch (const std::exception& e) {
      LOG_ERROR("SerializerManager::Deserialize error: {}", e.what());
      return false;
    }
  }

 private:
  template <typename T>
  static std::string Serialize_Json(const T& msg) {
    return JsonSerializer::Serialize(msg);
  }

  template <typename T>
  static bool Deserialize_Json(const std::string& msg, T& out) {
    return JsonSerializer::Deserialize(msg, out);
  }

  template <typename T>
  static std::string Serialize_Protobuf(const T& msg) {
    return ProtobufSerializer::Serialize(msg);
  }

  template <typename T>
  static bool Deserialize_Protobuf(const std::string& msg, T& out) {
    return ProtobufSerializer::Deserialize(msg, out);
  }
};
}  // namespace rpc
