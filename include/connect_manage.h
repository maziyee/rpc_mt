#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "connect.h"
namespace rpc {
class ConnectManage {
 public:
  static ConnectManage& GetInstance() {
    static ConnectManage instance;
    return instance;
  };
  ~ConnectManage();
  bool AddConnect(std::shared_ptr<Connect> connect);
  bool RemoveConnect(int fd);
  int GetConnectCount() { return m_connects.size(); };
  std::shared_ptr<Connect> GetConnect(int fd);

  void ClossAll();

 private:
  std::mutex mutex_;
  ConnectManage() = default;
  std::unordered_map<int, std::shared_ptr<Connect>> m_connects;
};
}  // namespace rpc