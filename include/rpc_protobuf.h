#pragma once

#include <stdint.h>

#include <string>

#include "serializer_mananger.h"

namespace rpc {

// 帧类型。放【内层】RpcHeader（密文里），不放外层帧头 —— 外层
// [magic][frame_len] 是明文，类型放那儿等于让对端随便标。
constexpr uint8_t kRpcTypeRequest = 0;
constexpr uint8_t kRpcTypeResponse = 1;

// RpcResponse::error_code_ 的取值。
//
// ⚠️ 类型是 uint32_t：SetErrorCode 收无符号，传 -1 进去会存成 4294967295 ——
//    谁写 `GetErrorCode() == -1` 都永远不成立。（原来 SendErrorRes 就是这么传的。）
//
// 分三档而不是"0 / 非 0"：调用方对「服务拒绝了」和「框架没处理」的处理方式不同 ——
// 前者去读 result_data_ 里的 {"error":...}，后者只能看 error_message。
constexpr uint32_t kRpcOk = 0;            // 成功
constexpr uint32_t kRpcErrService = 1;    // 服务拒绝了；result_data_ 通常非空
constexpr uint32_t kRpcErrFramework = 2;  // 请求没到服务层；result_data_ 可能为空

struct RpcHeader {
  uint32_t magic_;
  uint32_t body_size_;
  uint32_t sequence_id_;
  // 见上面两个常量。
  //
  // 没有它就无法分辨一条帧是 RpcRequest 还是 RpcResponse：两者的 body 布局
  // 不同，按错的类型解析【不会报错】，只会解出乱码。
  //
  // 3 个 uint32_t + 这个 uint8_t = 13 字节，sizeof 是 16（3 字节 padding）。
  // 两边都用 sizeof(RpcHeader) 裸 memcpy 所以一致，但 padding 是未初始化的
  // 栈内容、会被一起发上线 —— 构造时一律写 `RpcHeader header{};`。
  uint8_t type_;

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

  bool IsValid() const {
    return !service_name_.empty() && !method_name_.empty();
  }

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