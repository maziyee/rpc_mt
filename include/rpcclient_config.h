#pragma once

#include <string>

namespace rpc {
class RpcClientConfig {
 public:
  static RpcClientConfig& GetInstance() {
    static RpcClientConfig instance;
    return instance;
  }
  RpcClientConfig(const RpcClientConfig&) = delete;
  RpcClientConfig& operator=(const RpcClientConfig&) = delete;
  ~RpcClientConfig() = default;
  bool Init(const std::string& config_path);
  int GetTimeoutMs() const { return timeout_ms; }
  int GetRetryTimes() const { return retry_times; }
  int GetServerPort() const { return server_port; }
  std::string GetClientip() const { return client_ip; }

 private:
  void SetTimeoutMs(int timeout_ms) { this->timeout_ms = timeout_ms; }
  void SetRetryTimes(int retry_times) { this->retry_times = retry_times; }
  void SetServerPort(int server_port) { this->server_port = server_port; }
  void SetClientIp(const std::string& client_ip) {
    this->client_ip = client_ip;
  }
  RpcClientConfig() = default;

 private:
  int timeout_ms;
  int retry_times;
  int server_port;
  std::string client_ip;
};
}  // namespace rpc