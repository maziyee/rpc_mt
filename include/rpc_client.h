#pragma once

#include <netinet/in.h>

#include <atomic>
#include <string>
#include <vector>

#include "aes_encrypt.h"
#include "connect.h"
#include "log_manager.h"
#include "rpc_protobuf.h"
#include "serializer_mananger.h"
#include "zstd_compress.h"
namespace rpc {
class RpcClient {
 public:
  RpcClient(const std::string config_path, const std::string zk_config_path);
  ~RpcClient();

  template <typename Response, typename Request>
  bool Call(const std::string& service_name, const std::string& method_name,
            SerializerType serialzer_type, const Request& request,
            Response& response);
  bool Connect();
  void DisConnect();
  bool IsConnect() { return this->is_connected_; };

 private:
  bool InitConfig(const std::string& config_path,
                  const std::string& zk_config_path);
  bool TryConnect(int fd, struct sockaddr_in& server_addr, int retry_count);
  bool InitSocket(int& fd);

  uint32_t GenerateSequenceId() { return sequence_id_++; };
  template <typename Resquest>
  bool PrepareBeforeSent(const Resquest& request,
                         const std::string& service_name,
                         const std::string& method_name,
                         SerializerType serialize_type,
                         rpc::RpcRequest& set_request);

  bool SendRequest(rpc::RpcRequest& set_request);

  template <typename Response>
  bool ProcessResponse(Response& response, SerializerType serialize_type);

 private:
  int socket_fd_{-1};
  std::shared_ptr<rpc::Connect> conn_;
  std::atomic<bool> is_connected_{false};
  int retry_times_{10};
  int time_out_ms_{1000};
  std::string zk_namespace_;
  std::atomic<uint32_t> sequence_id_{0};
  std::mutex mutex_;
  std::string client_ip_;
};
template <typename Response, typename Request>
inline bool RpcClient::Call(const std::string& service_name,
                            const std::string& method_name,
                            SerializerType serialzer_type,
                            const Request& request, Response& response) {
  try {
    if (!this->is_connected_) {
      LOG_ERROR("client is not connected");
      return false;
    }
    rpc::RpcRequest rpc_request;
    if (!this->PrepareBeforeSent(request, service_name, method_name,
                                 serialzer_type, rpc_request)) {
      LOG_ERROR("PrepareBeforeSent error");
      return false;
    }
    if (!this->SendRequest(rpc_request)) {
      LOG_ERROR("SendRequest error");
      return false;
    }
    if (!this->ProcessResponse(response, serialzer_type)) {
      LOG_ERROR("ProcessResponse error");
      return false;
    }
  } catch (const std::exception& e) {
    LOG_ERROR("Call error: {}", e.what());
    return false;
  }
  return true;
}
template <typename Resquest>
inline bool RpcClient::PrepareBeforeSent(const Resquest& request,
                                         const std::string& service_name,
                                         const std::string& method_name,
                                         SerializerType serialize_type,
                                         rpc::RpcRequest& set_request) {
  std::string data_serialized =
      SerializerManager::Serialize(request, serialize_type);
  rpc::RpcRequest rpc_request;
  rpc_request.SetSequenceId(this->GenerateSequenceId());
  rpc_request.SetMethodName(method_name);
  rpc_request.SetServiceName(service_name);
  rpc_request.SetPayload(data_serialized);
  set_request = rpc_request;
  return true;
}
template <typename Response>
inline bool RpcClient::ProcessResponse(Response& response,
                                       SerializerType serialize_type) {
  if (!this->conn_->ReadWithTimeout(this->time_out_ms_)) {
    LOG_ERROR("ReadWithTimeout error");
    return false;
  }
  // 从读缓冲里提取【一条】完整帧（半包时 ConsumeFrame 返回 false）。
  // 用 ConsumeFrame 而不是 GetReadBuf()：后者返回拷贝、不消费缓冲，
  // 会让 recv_buf_ 无限累积 —— 这正是"第二次 Call 读到上次残留"的原因。
  std::string cipher;
  if (!this->conn_->ConsumeFrame(cipher)) {
    LOG_ERROR("no complete frame in read buffer");
    return false;
  }
  std::string decrypt_data;
  if (!AesEncrypt::GetInstance().Decrypt(cipher, decrypt_data)) {
    LOG_ERROR("Decrypt error");
    return false;
  }
  std::string decompress_data;
  if (!ZstdCompress::GetInstance().DecompressString(decrypt_data,
                                                    decompress_data)) {
    LOG_ERROR("Decompress error");
    return false;
  }
  rpc::RpcResponse rpc_response;
  if (!rpc_response.Deserializer(decompress_data)) {
    LOG_ERROR("Deserializer error");
    return false;
  }
  if (!rpc_response.GetResultData().empty()) {
    if (!SerializerManager::Deserialize(rpc_response.GetResultData(), response,
                                        serialize_type)) {
      LOG_ERROR("Deserialize error");
      return false;
    }
  }
  return true;
}
}  // namespace rpc