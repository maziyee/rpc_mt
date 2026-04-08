#include "rpc_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <string>

#include "rpcclient_config.h"
#include "zk_config.h"
#include "zk_handler.h"

rpc::RpcClient::~RpcClient() { DisConnect(); }

rpc::RpcClient::RpcClient(const std::string config_path,
                          const std::string zk_config_path) {
  try {
    if (!InitConfig(config_path, zk_config_path)) {
      throw std::runtime_error("InitConfig error");
    };
    if (!this->InitSocket(this->socket_fd_)) {
      throw std::runtime_error("InitSocket error");
    }
    if (!this->Connect()) {
      throw std::runtime_error("Connect error");
    }
  } catch (const std::exception& e) {
    LOG_ERROR("RpcClient init error: {}", e.what());
    throw;
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
