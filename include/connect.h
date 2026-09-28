#pragma once

#include <atomic>
#include <mutex>
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
  bool Write(RpcRequest &request);
  void Close();

  // send_buf_ 里是否还有没发完的残留字节。
  // 无锁（只读一个 atomic）—— 这个会被事件循环每次 EPOLLOUT 问一遍，
  // 加锁的话在高连接数下就是纯竞争。
  bool HasPendingSend() const { return this->has_pending_.load(); }

  // 只把残留字节再推一次，不追加新数据。
  //
  // 为什么不复用 Write()：Write 的职责是"序列化 + 分帧 + 追加 + 发送"，
  // 在重试路径上再走一遍会把同一帧追加两次。
  //
  // 失败语义和 Write 不同：这里的 false 是【致命】的（对端已经走了），
  // 调用方应该摘掉连接；而 EAGAIN 不算失败，只是"这次仍然写不完"，
  // 留给下一次 EPOLLOUT。
  bool FlushSendBuf();

  void SetMessageCallback(Message_Callback cb) {
    this->message_callback_ = cb;
  };
  void SetCloseCallback(Close_Callback cb) { this->close_callback_ = cb; };

  int GetFd() const { return fd_; }
  std::string GetIp() const { return this->ip_; };
  int GetPort() const { return this->port_; };
  bool IsRunning() const { return this->is_running_.load() && this->fd_ > 0; };
  // 从读缓冲里提取【一条】完整密文，消费掉已用的字节。
  // 返回 false 表示"还没凑够一条"（半包）——调用方应等下次可读，而不是当错误。
  bool ConsumeFrame(std::string& cipher);
  bool ProgressGetMessage();
  bool ReadWithTimeout(int timeout_ms);
  std::string GetReadBuf() { return this->recv_buf_; }

 private:
  bool SentBufInfo();
  std::string ReadTheInfo();

 private:
  int fd_;
  std::string ip_;
  int port_;
  std::string recv_buf_;
  std::vector<char> send_buf_;
  const int kMaxBufSize = 1024 * 1024;
  std::atomic<bool> is_running_;
  // send_buf_ 有没有残留。用 atomic 而不是裸 bool：HandleWrite 要无锁地
  // 先问一句"这条连接有东西没发完吗"，绝大多数时候答案是没有，不该为此加锁。
  std::atomic<bool> has_pending_{false};
  Message_Callback message_callback_;
  Close_Callback close_callback_;
  std::mutex write_mutex_;
};

}  // namespace rpc