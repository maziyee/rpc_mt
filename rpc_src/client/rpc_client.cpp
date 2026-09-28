#include "rpc_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <string>

#include "load_balance.h"
#include "rpcclient_config.h"
#include "zk_config.h"
#include "zk_handler.h"

rpc::RpcClient::~RpcClient() { DisConnect(); }

rpc::RpcClient::RpcClient(const std::string config_path,
                          const std::string zk_config_path) {
  // 配置错 = 调用方没准备好，属于编程/部署错误 → 抛
  if (!InitConfig(config_path, zk_config_path)) {
    LOG_ERROR("RpcClient: init config failed");
    throw std::runtime_error("InitConfig error");
  }
  // socket() 都建不出来，属于资源问题 → 也抛
  if (!this->InitSocket(this->socket_fd_)) {
    LOG_ERROR("RpcClient: init socket failed");
    throw std::runtime_error("InitSocket error");
  }

  // ⚠️ Connect 失败【不抛】。
  //
  // "服务端还没起来"是个正常状态，不是编程错误。而且 Call() 里本来就有
  // `if (!is_connected_) return false;` 专门处理这条路 —— 构造函数抛异常
  // 等于绕过了它，强迫每个调用方包 try/catch。
  //
  // 连不上时客户端就停在"未连接"状态，调用方用 IsConnect() 判断，
  // 或者直接 Call() 拿 false。
  if (!this->Connect()) {
    LOG_WARN("RpcClient: not connected (server down?), IsConnect() is false");
  }
}

bool rpc::RpcClient::Connect() {
  std::lock_guard<std::mutex> lock(this->mutex_);
  if (this->is_connected_) {
    LOG_INFO("already connected");
    return true;
  }
  std::string server_ip_and_port =
      ZkHandler::GetInstance().GetServer(this->zk_namespace_, this->client_ip_);
  if (server_ip_and_port.empty()) {
    LOG_ERROR("GetServer error");
    return false;
  }
  std::string server_ip =
      server_ip_and_port.substr(0, server_ip_and_port.find(":"));
  int server_port =
      std::stoi(server_ip_and_port.substr(server_ip_and_port.find(":") + 1));
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(server_port);
  server_addr.sin_addr.s_addr = inet_addr(server_ip.c_str());
  int retry_count = 0;
  while (retry_count < this->retry_times_) {
    int fd;
    if (!InitSocket(fd)) {
      LOG_ERROR("InitSocket error");
      retry_count++;
      continue;
    }
    if (!TryConnect(fd, server_addr, retry_count)) {
      LOG_ERROR("TryConnect error");
      close(fd);
      retry_count++;
      continue;
    }
    this->conn_ = std::make_shared<rpc::Connect>(fd);
    break;
  }

  // ⚠️ 原来这里无条件 is_connected_ = true 并返回 true。重试全部失败时
  //    conn_ 仍然是空的，但标志位宣称"已连接" —— 下一次 Call 会通过
  //    is_connected_ 检查，然后在 SendRequest 里 this->conn_->Write(...)
  //    空指针解引用，直接段错误。
  //
  //    触发场景很平常：服务端没起来时跑客户端。
  if (!this->conn_) {
    LOG_ERROR("Connect failed: retried {} times", this->retry_times_);
    return false;
  }
  this->is_connected_ = true;
  return true;
}
void rpc::RpcClient::DisConnect() {
  if (!this->IsConnect()) {
    LOG_INFO("already disconnected");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(this->mutex_);
    this->is_connected_ = false;
    this->conn_->Close();
    this->conn_.reset();
    this->socket_fd_ = -1;
    LOG_INFO("DisConnect success");
  }
}
bool rpc::RpcClient::InitConfig(const std::string& config_path,
                                const std::string& zk_config_path) {
  auto& config = RpcClientConfig::GetInstance();
  if (!config.Init(config_path)) {
    LOG_ERROR("Init RpcClientConfig error");
    return false;
  }
  this->retry_times_ = config.GetRetryTimes();
  this->time_out_ms_ = config.GetTimeoutMs();
  this->client_ip_ = config.GetClientip();

  // 按配置选择负载均衡策略；未配置或配置非法时 InitLoadBanlance 内部会退回 random
  const std::string& balance_type = config.GetLoadBalance();
  if (!LoadBanlance::InitLoadBanlance(balance_type)) {
    LOG_WARN("InitLoadBanlance failed for type '{}', fallback to random",
             balance_type);
  }

  ZkConfig zk;
  zk.InitZkConfig(zk_config_path);
  if (!ZkHandler::GetInstance().InitZkHandler(&zk)) {
    LOG_ERROR("InitZkHandler error");
    return false;
  }
  return true;
}

bool rpc::RpcClient::TryConnect(int fd, struct sockaddr_in& server_addr,
                                int retry_count) {
  int ret = connect(fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
  if (ret == 0) {
    LOG_INFO("connect success");
    return true;
  }
  if (errno != EINPROGRESS && errno != EALREADY) {
    LOG_ERROR("connect error: {}", strerror(errno));
    return false;
  }
  fd_set write_fds;
  struct timeval timeout;
  timeout.tv_sec = this->time_out_ms_ / 1000;
  timeout.tv_usec = (this->time_out_ms_ % 1000) * 1000;

  FD_ZERO(&write_fds);
  FD_SET(fd, &write_fds);
  ret = select(fd + 1, nullptr, &write_fds, nullptr, &timeout);
  if (ret < 0) {
    LOG_ERROR("select error: {}", strerror(errno));
    return false;
  }
  if (ret == 0) {
    LOG_ERROR("connect timeout");
    return false;
  }

  int error = 0;
  socklen_t len = sizeof(error);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
    LOG_ERROR("getsockopt error: {}", strerror(errno));
    return false;
  }
  if (error != 0) {
    LOG_ERROR("connect error: {}", strerror(error));
    return false;
  }
  return true;
}

bool rpc::RpcClient::InitSocket(int& fd) {
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    LOG_ERROR("socket error: {}", strerror(errno));
    return false;
  }
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    LOG_ERROR("fcntl error: {}", strerror(errno));
    close(fd);
    return false;
  }
  LOG_INFO("socket init success");
  return true;
}

bool rpc::RpcClient::SendRequest(rpc::RpcRequest& set_request) {
  if (!this->IsConnect()) {
    LOG_ERROR("not connected");
    return false;
  };
  if (!this->conn_->Write(set_request)) {
    LOG_ERROR("Write error");
    return false;
  }
  return true;
}
