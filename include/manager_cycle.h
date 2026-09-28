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
  // 只把停止标志置真，不做任何别的事 —— 所以它可以在【信号处理函数】里调。
  //
  // 和 Stop() 的区别是关键的：Stop() 要拿 ConnectManage::mutex_ 和 spdlog 的
  // 锁、还要遍历容器。信号处理函数里做这些，一旦打断了正持有同一把锁的线程
  // 就是永久自死锁（std::mutex 不可重入），而且死在 handler 里连 Ctrl+C 都
  // 救不回来。真正置标志是 async-signal-safe 的；清理必须回主线程做。
  void RequestStop();
  void HandleEvent(int fd, uint32_t events);
  void HandleNewConnection(Socket* socket);
  void HandleData(int fd);
  // 把某条连接 send_buf_ 里的残留字节再推一次（EPOLLOUT 事件驱动）。
  // 公开是为了能单独测——推送路径上没有"下一次 Write"来兜底，这里是唯一出口。
  void HandleWrite(int fd);
  void RemoveConnect(int fd);
  void HandleMessage(const std::shared_ptr<Connect>& connect,
                     const rpc::RpcRequest& request);
  void HandleClose(const std::shared_ptr<Connect>& connect);
  bool AddListenFd(int fd, uint32_t events, Socket* socket);

 private:
  void Create();

  void Add(int fd, uint32_t events);
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