#include <signal.h>

#include <atomic>
#include <filesystem>
#include <iostream>

#include "aes_encrypt.h"
#include "log_manager.h"
#include "manager_cycle.h"
#include "rpc_config_mananger.h"
#include "service_registry.h"
#include "socket.h"
#include "thread_pool.h"
#include "thread_single.h"
#include "zk_handler.h"

static std::atomic<bool> g_shutdown_requested{false};
static rpc::ManagerCycle* g_manager_cycle = nullptr;
static std::shared_ptr<rpc::Socket> g_socket_server = nullptr;

static std::atomic<int> g_received_signal{0};

void SignHandle(int sig) {
  g_received_signal.store(sig, std::memory_order_relaxed);
  g_shutdown_requested = true;
  if (g_manager_cycle) {
    g_manager_cycle->Stop();
  }
}

void GreacefulShutdown() {
  LOG_INFO("Starting graceful shutdown...");
  if (g_socket_server) {
    LOG_INFO("Closing server socket...");
    g_socket_server.reset();
  }
  LOG_INFO("Closing all connections...");
  auto& connection_manager = rpc::ConnectManage::GetInstance();
  connection_manager.CloseAll();
  LOG_INFO("Shutting down ThreadPool...");
  if (meeting_ctrl::ThreadSingle::GetStatus() ==
      meeting_ctrl::ThreadStatus::kRunning) {
    if (meeting_ctrl::ThreadSingle::ShutDown()) {
      LOG_INFO("ThreadPool shut down successfully");
    } else {
      LOG_WARN("ThreadPool shutdown timeout, forcing stop");
      meeting_ctrl::ThreadSingle::Stop_Now();
    }
  }
  try {
    auto& zk_handler = rpc::ZkHandler::GetInstance();
    zk_handler.CleanUp();
  } catch (std::exception& e) {
    LOG_ERROR("ShutDown error: {}", e.what());
  }
  spdlog::shutdown();
}

int main() {
  signal(SIGINT, SignHandle);
  signal(SIGTERM, SignHandle);

  std::atexit([]() {
    if (!g_shutdown_requested.load()) {
      LOG_ERROR("main exit before shutdown");
      GreacefulShutdown();
    }
  });

  try {
    std::filesystem::path exe_dir =
        std::filesystem::canonical("/proc/self/exe").parent_path();
    std::filesystem::path config_dir = exe_dir / "../config";
    std::cerr << "exe_dir: " << exe_dir << "\nconfig_dir: "
              << std::filesystem::weakly_canonical(config_dir) << std::endl;
    rpc::RpcConfigManager::GetInstance().Init(
        (config_dir / "spdlog_config.json").string(),
        (config_dir / "service_config.json").string(),
        (config_dir / "thread_pool_aes_config.json").string(),
        (config_dir / "service_socket_config.json").string(),
        (config_dir / "zk_config.json").string());

    // 初始化线程池
    if (!meeting_ctrl::ThreadSingle::Init(rpc::RpcConfigManager::GetInstance()
                                              .GetThreadAesConfig()
                                              ->GetThreadConfig())) {
      LOG_ERROR("Init ThreadPool error");
    };
    // 初始化服务器
    auto service_socket_config =
        rpc::RpcConfigManager::GetInstance().GetServiceSocketConfig();
    std::string server_ip = service_socket_config->GetServiceIp();
    uint16_t server_port = service_socket_config->GetServicePort();
    uint16_t max_connection = service_socket_config->GetServiceMaxConnections();
    uint16_t timeout = service_socket_config->GetServiceThreadTimeout();

    g_socket_server = std::make_shared<rpc::Socket>(server_ip, server_port,
                                                    max_connection, timeout);
    // 初始化加密模块

    rpc::AesEncrypt::GetInstance().Init(rpc::RpcConfigManager::GetInstance()
                                            .GetThreadAesConfig()
                                            ->GetMasterKey());

    auto& conn_manager = rpc::ConnectManage::GetInstance();
    auto manager_cycle = std::make_shared<rpc::ManagerCycle>(&conn_manager);
    if (!manager_cycle) {
      LOG_ERROR("Init ManagerCycle error");
      return -1;
    }
    if (!manager_cycle->AddListenFd(g_socket_server.get()->GetFd(),
                                    EPOLLIN | EPOLLOUT | EPOLLRDHUP,
                                    g_socket_server.get())) {
      LOG_ERROR("AddListenFd error");
      return -1;
    };

    auto& service_manager = rpc::ServiceManager::GetInstance();
    auto user_service = std::make_shared<rpc::UserService>();
    auto rpc_service = std::make_shared<rpc::RpcService>(user_service);
    if (!service_manager.RegisterService(user_service) ||
        !service_manager.RegisterService(rpc_service)) {
      LOG_ERROR("RegisterService error");
      return -1;
    };

    auto& zk_handler = rpc::ZkHandler::GetInstance();
    if (!zk_handler.InitZkHandler(
            rpc::RpcConfigManager::GetInstance().GetZkConfig())) {
      std::cerr << "zk_handler init failed" << std::endl;
      return -1;
    }

    if (!zk_handler.RegistryAllNode(
            rpc::RpcConfigManager::GetInstance().GetServiceConfig())) {
      std::cerr << "zk_handler registry failed" << std::endl;
      return -1;
    }
    g_manager_cycle = manager_cycle.get();
    manager_cycle->Loop();

    g_shutdown_requested = true;
    GreacefulShutdown();
    return 0;
  } catch (std::exception& e) {
    LOG_ERROR("Init RpcConfigManager error: {}", e.what());
    GreacefulShutdown();
    return -1;
  } catch (...) {
    LOG_ERROR("Init RpcConfigManager error: unknown error");
    GreacefulShutdown();
    return -1;
  }
  g_shutdown_requested = true;
  GreacefulShutdown();
  return 0;
}