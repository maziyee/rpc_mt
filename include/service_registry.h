#pragma once

#include <zookeeper/zookeeper.h>

#include <memory>
#include <string>
#include <vector>

#include "log_manager.h"

class ServiceRegistry {
 public:
  ServiceRegistry(const std::string& zk_hosts);
  ~ServiceRegistry();

  bool Register(const std::string& service_name,
                const std::string& service_addr);
  bool IsConnected() const;

 private:
  static void GlobalWatcher(zhandle_t* zk_handle, int type, int state,
                            const char* path, void* watcher_ctx);
  bool EnSurePath(const std::string& path);
  bool CreateNode(const std::string& path, const std::string& value, int flags);

 private:
  zhandle_t* zk_handle_;
  static const std::string ROOT_PATH;
  bool is_connected_;
};