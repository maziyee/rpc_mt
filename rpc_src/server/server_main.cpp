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

// ⚠️ 信号处理函数里【只能】做 async-signal-safe 的事。
//
// 这里只做两件：存两个 atomic，加上调 RequestStop()（那也只是存一个 atomic）。
//
// 原来这里直接调的是 Stop()，那是错的：Stop() 要拿 ConnectManage::mutex_ 和
// spdlog 的锁、还要遍历 unordered_map。信号一旦打断正持有同一把锁的线程，
// handler 就会在锁上等【它自己】—— std::mutex 不可重入 → 永久死锁。而且死在
// 信号处理函数里，Ctrl+C 也救不回来（SIGINT 走另一个 handler，但循环已经不转了）。
//
// 置标志本身是安全的；真正的清理回 main 线程做（GreacefulShutdown 和析构）。
void SignHandle(int sig) {
  g_received_signal.store(sig, std::memory_order_relaxed);
  g_shutdown_requested = true;
  if (g_manager_cycle) {
    g_manager_cycle->RequestStop();
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

  // ⚠️ 这里【不能】调 spdlog::shutdown()。
  //
  // 调用它之后，spdlog::default_logger() 变成 nullptr，任何 LOG_* 都会在
  // should_log() 里空指针解引用、直接段错误。而关闭流程结束不等于对象都析构
  // 完了 —— main 里的局部对象（ManagerCycle 等）是 main 返回时才析构的，
  // 比这里还晚，而 ~ManagerCycle / ~Connect / ~ConnectManage 里都有 LOG_*。
  //
  // 症状就是服务正常跑、正常响应，一收到 SIGTERM 就 139 (SIGSEGV)，
  // 而且崩在析构路径上、看起来像"退出时本来就乱"。
  //
  // 不显式关闭没有代价：logger 设了 flush_on(trace)，每条日志都已经落盘，
  // 剩下的资源由 spdlog 自己的静态析构收尾 —— 那个时机在最后，比所有
  // 会打日志的对象都晚。
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

    // ⚠️ 信号可能在上面那行【之前】就到达。那时 SignHandle 里的
    //    `if (g_manager_cycle)` 还是 null，只能设 g_shutdown_requested，
    //    调不到 Stop() —— 而 Stop() 才是把 is_stop_ 置真的那个。
    //    结果就是 Loop() 永远跑下去，进程对 SIGTERM 完全免疫。
    //
    //    在赋值和进循环之间补一次检查，把这个窗口关掉。
    //    （赋值【之后】来的信号是安全的：SignHandle 会调 Stop()，is_stop_
    //      已经是 true，Loop() 立刻返回。）
    if (g_shutdown_requested.load()) {
      LOG_INFO("shutdown requested before loop started, skipping loop");
      manager_cycle->Stop();
    }
    manager_cycle->Loop();

    // ⚠️ Loop 返回后必须立刻断开这个【裸指针】。
    //    从这一刻起到 main 结束之间，manager_cycle 这个 shared_ptr 随时可能
    //    析构；而 SignHandle 里是 `if (g_manager_cycle) g_manager_cycle->...`——
    //    不断开的话，之后到达的任何信号都会调到一个已释放的对象上。
    g_manager_cycle = nullptr;

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