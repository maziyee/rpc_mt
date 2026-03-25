#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "log_manager.h"
#include "rpc_protobuf.h"

namespace rpc {
class RpcHeader;
class RpcRequest;
class RpcResponse;
class Connect : public std::enable_shared_from_this<Connect> {
 public:
  using Message_Callback =
      std::function<void(const std::shared_ptr<Connect> &, RpcRequest &)>;
  using Close_Callback = std::function<void(const std::shared_ptr<Connect> &)>;
  explicit Connect(int fd);
  ~Connect();
  bool Read();
  bool Write(RpcResponse &response);
  void Close();

  void SetMessageCallback(Message_Callback cb) {
    this->message_callback_ = cb;
  };
  void SetCloseCallback(Close_Callback cb) { this->close_callback_ = cb; };

  int GetFd() const { return fd_; }
  std::string GetIp() const { return this->ip_; };
  int GetPort() const { return this->port_; };
  bool IsRunning() const { return this->is_running_.load() && this->fd_ > 0; };
  bool ProgressGetMessage();

 private:
  bool SentBufInfo();
  std::string ReadTheInfo();

 private:
  int fd_;
  std::string ip_;
  int port_;
  std::vector<char> recv_buf_;
  std::vector<char> send_buf_;
  const int kMaxBufSize = 1024 * 1024;
  std::atomic<bool> is_running_;
  Message_Callback message_callback_;
  Close_Callback close_callback_;
};

}  // namespace rpc