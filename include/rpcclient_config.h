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
  std::string GetClientip() const { return client_ip; }
  std::string GetLoadBalance() const { return load_balance; }

 private:
  void SetTimeoutMs(int timeout_ms) { this->timeout_ms = timeout_ms; }
  void SetRetryTimes(int retry_times) { this->retry_times = retry_times; }
  void SetClientIp(const std::string& client_ip) {
    this->client_ip = client_ip;
  }
  void SetLoadBalance(const std::string& load_balance) {
    this->load_balance = load_balance;
  }
  RpcClientConfig() = default;

 private:
  int timeout_ms = 1000;
  int retry_times = 3;
  std::string client_ip;
  std::string load_balance = "random";
};
}  // namespace rpc