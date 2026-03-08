#pragma once

#include <stdint.h>

#include <string>

#include "serializer_mananger.h"

namespace rpc {

struct RpcHeader {
  uint32_t magic_;
  uint32_t body_size_;
  uint32_t sequence_id_;

  static const uint32_t kMagic;
};

class RpcBase {
 public:
  virtual ~RpcBase() = default;
  virtual bool Serializer(std::string& out) = 0;
  virtual bool Deserializer(const std::string& in) = 0;

  uint32_t GetSequenceId() const { return sequence_id_; }
  void SetSequenceId(uint32_t sequence_id) { sequence_id_ = sequence_id; }

 private:
  uint32_t sequence_id_;
};

class RpcRequest : public RpcBase {
 public:
  std::string GetServiceName() const { return service_name_; }
  void SetServiceName(const std::string& service_name) {
    service_name_ = service_name;
  }

  std::string GetMethodName() const { return method_name_; }
  void SetMethodName(const std::string& method_name) {
    method_name_ = method_name;
  }

  std::string GetPayload() const { return payload_; }
  void SetPayload(const std::string& payload) { payload_ = payload; }

  bool Serializer(std::string& out) override;
  bool Deserializer(const std::string& in) override;

 private:
  std::string service_name_;
  std::string method_name_;
  std::string payload_;
};

class RpcResponse : public RpcBase {
 public:
  std::string GetErrorMessage() const { return error_message_; }
  void SetErrorMessage(const std::string& error_message) {
    error_message_ = error_message;
  }
  uint32_t GetErrorCode() const { return error_code_; }
  void SetErrorCode(uint32_t error_code) { error_code_ = error_code; }
  std::string GetResultData() const { return result_data_; }
  void SetResultData(const std::string& result_data) {
    result_data_ = result_data;
  }

  bool Serializer(std::string& out) override;
  bool Deserializer(const std::string& in) override;

 private:
  uint32_t error_code_;
  std::string error_message_;
  std::string result_data_;
};
}  // namespace rpc