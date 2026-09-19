#pragma once

#include <zookeeper/zookeeper.h>

#include <string>

#include "log_manager.h"
#include "zk_handle.h"

namespace rpc {

// 只负责「往 ZK 写」：注册服务实例（临时节点）与建父路径。
// 连接不由自己建立 —— 借用 ZkHandler 已经建好的那条，双方共享同一 session。
class ServiceRegistry {
 public:
  explicit ServiceRegistry(ZkHandle zk_handle);

  bool Register(const std::string& service_name,
                const std::string& service_addr);
  bool IsConnected() const;

 private:
  bool EnSurePath(const std::string& path);
  bool CreateNode(const std::string& path, const std::string& value, int flags);

 private:
  ZkHandle zk_handle_;
  static const std::string ROOT_PATH;
};

}  // namespace rpc
