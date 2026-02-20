#include "zk_handler.h"

void rpc::ZkHandler::UpdateServers() {
  auto new_servers = this->GetServers();
  {
    std::lock_guard<std::mutex> lock(this->server_mutex_);
    this->servers_ = std::move(new_servers);
  }
}

std::string rpc::ZkHandler::GetServer(const std::string& zk_namespace,
                                      std::string& client_ip) {
  this->UpdateServers();
  if (this->servers_.empty()) {
    LOG_ERROR("GetServer failed: no servers");
    return "";
  }
  std::string server =
      LoadBanlance::GetInstance()->SelectServers(this->servers_, client_ip);
  return server;
}

void rpc::ZkHandler::CleanUp() {
  static std::atomic<bool> is_clean_up(false);
  if (is_clean_up.load()) {
    return;
  }
  is_clean_up.store(true);
  this->service_registry_.reset();

  if (this->zk_client) {
    try {
      zookeeper_close(this->zk_client);
      zk_client = nullptr;
    } catch (const std::exception& e) {
      LOG_ERROR("zookeeper_close failed: {}", e.what());
      zk_client = nullptr;
      return;
    } catch (...) {
      LOG_ERROR("zookeeper_close failed: unknown error");
      zk_client = nullptr;
      return;
    }
  }
  this->is_running_.store(false);
}

rpc::ZkHandler::~ZkHandler() { CleanUp(); }

void rpc::ZkHandler::GlobalWatcher(zhandle_t* zh, int type, int state,
                                   const char* path, void* watcherCtx) {
  if (watcherCtx == nullptr) {
    return;
  }
  ZkHandler* handler = static_cast<ZkHandler*>(watcherCtx);
  if (type == ZOO_SESSION_EVENT) {
    if (state == ZOO_CONNECTED_STATE) {
      LOG_INFO("zkhandler connected");
    } else if (state == ZOO_EXPIRED_SESSION_STATE) {
      LOG_ERROR("zkhandler session expired");
    } else if (state == ZOO_AUTH_FAILED_STATE) {
      LOG_ERROR("zkhandler auth failed");
    } else {
      LOG_WARN("zkhandler unknown state {}", state);
    }
  }
}

bool rpc::ZkHandler::CreateRegistry() {
  try {
    auto zk_connect = this->zk_host_ + ":" + std::to_string(this->zk_port_);
    this->service_registry_ = std::make_unique<ServiceRegistry>(zk_connect);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (!this->service_registry_->IsConnected()) {
      LOG_ERROR("CreateRegistry failed: not connected");
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    LOG_ERROR("CreateRegistry failed: {}", e.what());
    return false;
  }
}

ServiceRegistry* rpc::ZkHandler::GetServiceRegistry() {
  if (this->service_registry_ == nullptr) {
    std::lock_guard<std::mutex> lock(this->mutex_);
    if (this->service_registry_ == nullptr) {
      if (!this->CreateRegistry()) {
        LOG_ERROR("GetServiceRegistry failed: create registry failed");
        return nullptr;
      }
    }
  }
  return this->service_registry_.get();
}

bool rpc::ZkHandler::RegistryANode(const std::string& service_name,
                                   const std::string& service_addr) {
  auto registry = this->GetServiceRegistry();
  if (registry == nullptr) {
    LOG_ERROR("RegistryANode failed: get registry failed");
    return false;
  }
  if (!registry->Register(service_name, service_addr)) {
    LOG_ERROR("RegistryANode failed: register failed");
    return false;
  }
  return true;
}

bool rpc::ZkHandler::RegistryAllNode(const ServiceConfig* service_config) {
  if (service_config == nullptr || service_config->GetRegistryNodeSize() == 0) {
    LOG_WARN("RegistryAllNode failed: no registry node");
    return true;
  }
  try {
    auto registry = this->GetServiceRegistry();
    if (registry == nullptr) {
      LOG_ERROR("RegistryAllNode failed: get registry failed");
      return false;
    }
    const auto& service_name = service_config->GetServiceName();
    bool all_success = true;
    for (const auto& node : service_config->GetRegistryNodes()) {
      auto service_addr = node.address + ":" + std::to_string(node.port);
      if (!this->RegistryANode(service_name, service_addr)) {
        LOG_ERROR("RegistryANode failed: register failed");
        all_success = false;
      }
    }
    return all_success;
  } catch (const std::exception& e) {
    LOG_ERROR("RegistryAllNode failed: {}", e.what());
    return false;
  }
}

std::vector<std::string> rpc::ZkHandler::GetServers() {
  if (!this->EnSureConnect()) {
    LOG_ERROR("GetServers failed: not connected");
    return {};
  }
  std::lock_guard<std::mutex> lock(this->server_mutex_);
  try {
    struct String_vector nodes = {0};

    int rc = zoo_get_children(this->zk_client, this->zk_namespace_.c_str(), 0,
                              &nodes);
    if (rc != ZOK) {
      LOG_ERROR("zoo_get_children failed: {}", rc);
      return {};
    }
    std::vector<std::string> servers;
    for (int i = 0; i < nodes.count; i++) {
      std::string node_path = zk_namespace_ + "/" + nodes.data[i];
      char data[1024];
      int data_len = sizeof(data);
      rc = zoo_get(this->zk_client, node_path.c_str(), 0, data, &data_len,
                   nullptr);
      if (rc == ZOK && data_len > 0) {
        std::string server(data, data_len);
        servers.push_back(server);
      } else {
        LOG_WARN("zoo_get failed: {}", rc);
      }
    }
    deallocate_String_vector(&nodes);
    return servers;
  } catch (const std::exception& e) {
    LOG_ERROR("GetServers failed: {}", e.what());
    return {};
  }
}

bool rpc::ZkHandler::InitZkHandler(nlohmann::json& zk_config) {
  try {
    if (zk_config.empty()) {
      LOG_ERROR("zk_config is empty");
      return false;
    }
    try {
      std::string zk_host = zk_config.value("zk_host", "localhost");
      SetZkHost(zk_host);

      int zk_port = zk_config.value("zk_port", 2181);
      SetZkport(zk_port);

      std::string zk_namespace = zk_config.value("zk_namespace", "mt_rpc");
      SetZkNamespace(zk_namespace);

      int retry_interval = zk_config.value("retry_interval", 1);
      SetRetryInterval(retry_interval);
    } catch (const std::exception& e) {
      LOG_ERROR("InitZkHandler failed: {}", e.what());
      return false;
    } catch (nlohmann::json::exception& e) {
      LOG_ERROR("InitZkHandler failed: {}", e.what());
      return false;
    }

    bool is_connected = EnSureConnect();
    if (!is_connected) {
      LOG_ERROR("InitZkHandler failed: not connected");
      return false;
    }
    if (this->zk_client == nullptr) {
      LOG_ERROR("InitZkHandler failed: zk_client is nullptr");
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    LOG_ERROR("InitZkHandler failed: {}", e.what());
    CleanUp();
    return false;
  }
}

void rpc::ZkHandler::SetZkport(int port) {
  std::lock_guard<std::mutex> lock(this->mutex_);
  if (port < 0 || port > 65535) {
    LOG_ERROR("Zk port is invalid: {}", port);
    this->zk_port_ = kDefultZkPort;
  }
  this->zk_port_ = port;
}

void rpc::ZkHandler::SetZkHost(const std::string& host) {
  std::lock_guard<std::mutex> lock(this->mutex_);
  if (host.empty()) {
    LOG_ERROR("Zk host is empty");
    this->zk_host_ = "localhost";
  } else {
    this->zk_host_ = host;
  }
}

void rpc::ZkHandler::SetZkNamespace(const std::string& zk_namespace) {
  this->zk_namespace_ = zk_namespace;
}

void rpc::ZkHandler::SetRetryInterval(const int seconds) {
  std::lock_guard<std::mutex> lock(this->mutex_);
  if (seconds < 0) {
    LOG_ERROR("the interval is invaild");
    this->retry_interval_ = std::chrono::seconds(1);
  }
  this->retry_interval_ = std::chrono::seconds(seconds);
}

bool rpc::ZkHandler::EnSureConnect() {
  if (!this->zk_client) {
    try {
      std::string conn_string =
          this->zk_host_ + ":" + std::to_string(this->zk_port_);
      this->zk_client = zookeeper_init(conn_string.c_str(), this->GlobalWatcher,
                                       30000, nullptr, this, 0);
      if (!this->zk_client) {
        LOG_ERROR("zk_handler init failed");
        return false;
      }
    } catch (const std::exception& e) {
      LOG_ERROR("zookeeper_init failed: {}", e.what());
      return false;
    } catch (...) {
      LOG_ERROR("zookeeper_init failed: unknown error");
      return false;
    }
  }
  int retry = 0;
  while (retry < kMaxRetryTimes) {
    int state = zoo_state(this->zk_client);
    const char* err_msg = nullptr;
    if (state == ZOO_CONNECTED_STATE) {
      err_msg = "ZOO_CONNECTED_STATE";
      LOG_INFO("zk_handler init success , state: {}", err_msg);
      return true;
    } else if (state == ZOO_CONNECTING_STATE) {
      err_msg = "ZOO_CONNECTING_STATE";
      LOG_INFO("zk_handler state: {}", err_msg);
    } else if (state == ZOO_EXPIRED_SESSION_STATE) {
      err_msg = "ZOO_EXPIRED_SESSION_STATE";
      LOG_ERROR("zk_handler connect failed, state: {}", err_msg);
      this->CleanUp();
      return false;
    } else if (state == ZOO_AUTH_FAILED_STATE) {
      err_msg = "AUTH_FAILED";
      LOG_ERROR("zk_handler connect failed, state: {}", err_msg);
      this->CleanUp();
      return false;
    } else {
      err_msg = "UNKNOWN_STATE";
      LOG_WARN("Unknown state: {}", state);
      if (this->zk_client) {
        struct Stat stat = {0};
        int rc = zoo_exists(zk_client, "/", 0, &stat);
        LOG_WARN("zoo_exists rc: {}", rc);
      }
    }
    if (state != ZOO_CONNECTING_STATE && retry > 15) {
      LOG_ERROR("zk_handler connect failed, state: {}", err_msg);
      this->CleanUp();
      return false;
    }
    std::this_thread::sleep_for(this->retry_interval_);
    retry++;
  }
  LOG_ERROR("zk_handler connect failed after retry {}", retry);
  this->CleanUp();
  return false;
}
