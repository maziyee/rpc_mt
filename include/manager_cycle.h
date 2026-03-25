#pragma once

#include <sys/epoll.h>

#include <atomic>
#include <mutex>
#include <unordered_map>

#include "aes_encrypt.h"
#include "connect_manage.h"
#include "log_manager.h"
#include "service_manager.h"
#include "socket.h"
#include "thread_single.h"
#include "zstd_compress.h"

namespace rpc {
class ManagerCycle {
 public:
  ManagerCycle(ConnectManage* manager);
  ~ManagerCycle();
  void Loop();
  void Stop();
  void HandleEvent(int fd, uint32_t events);
  void HandleNewConnection(Socket* socket);
  void HandleData(int fd);
  void RemoveConnect(int fd);
  void HandleMessage(const std::shared_ptr<Connect>& connect,
                     const rpc::RpcRequest& request);
  void HandleClose(const std::shared_ptr<Connect>& connect);

 private:
  void Create();
  bool AddListenFd(int fd, uint32_t events);
  void Modify(int fd, uint32_t events);
  void Remove(int fd);
  std::vector<struct epoll_event> Wait(int timeout_ms);

  void HandleMessageAsync(const std::shared_ptr<Connect>& connect,
                          const rpc::RpcRequest& request);

  void HandleMessageSync(const std::shared_ptr<Connect>& connect,
                         const rpc::RpcRequest& request);

  bool SendErrorRes(const std::shared_ptr<Connect>& connect, int sequenceid,
                    int error_code, std::string error_message);
  bool SendSuccessRes(const std::shared_ptr<Connect>& connect, int sequenceid,
                      std::string& result);

  bool SendRes(const std::shared_ptr<Connect>& connect,
               rpc::RpcResponse& response);

 private:
  int epoll_fd_;
  const int kMAX_EVENTS = 1024;
  std::vector<epoll_event> events_;
  ConnectManage* connect_manage_;
  std::atomic<bool> is_running_;
  std::mutex mutex_;
  std::unordered_map<int, Socket*> listen_fds_;
};
}  // namespace rpc