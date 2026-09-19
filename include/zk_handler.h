#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <zookeeper/proto.h>
#include <zookeeper/zookeeper.h>
#include <zookeeper/zookeeper_version.h>

#ifdef __cplusplus
}
#endif

#include <atomic>
#include <chrono>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "load_balance.h"
#include "log_manager.h"
#include "rpc_config_mananger.h"
#include "service_registry.h"
#include "zk_handle.h"

namespace rpc {
class RpcConfigManager;

class ZkHandler {
 public:
  static ZkHandler& GetInstance() {
    static ZkHandler instance;
    return instance;
  }

  ZkHandler(const ZkHandler&) = delete;
  ZkHandler& operator=(const ZkHandler&) = delete;
  ZkHandler(ZkHandler&&) = delete;
  ZkHandler& operator=(ZkHandler&&) = delete;

  bool InitZkHandler(ZkConfig* zk_config_);
  void SetZkport(int port);
  void SetZkHost(const std::string& host);
  void SetZkNamespace(const std::string& zk_namespace);
  void SetRetryInterval(const int interval);

  bool EnSureConnect();

  void UpdateServers();

  std::string GetServer(const std::string& zk_namespace,
                        std::string& client_ip);

  void CleanUp();

  bool CreateRegistry();

  ServiceRegistry* GetServiceRegistry();

  bool RegistryANode(const std::string& service_name,
                     const std::string& service_addr);

  bool RegistryAllNode(const ServiceConfig* service_config);

  std::vector<std::string> GetServers();
  ~ZkHandler();

 private:
  ZkHandler()
      : zk_client(nullptr), is_running_(false), service_registry_(nullptr){};

  static void GlobalWatcher(zhandle_t* zh, int type, int state,
                            const char* path, void* watcherCtx);

 private:
  std::atomic<bool> is_running_;
  std::chrono::seconds retry_interval_;
  std::string zk_host_;
  std::string zk_namespace_;
  int zk_port_;
  ZkHandle zk_client;

  std::mutex mutex_;
  std::mutex server_mutex_;
  std::vector<std::string> servers_;

  std::unique_ptr<ServiceRegistry> service_registry_;

  const int kDefultZkPort = 2181;
  const int kMaxRetryTimes = 10;
};
}  // namespace rpc