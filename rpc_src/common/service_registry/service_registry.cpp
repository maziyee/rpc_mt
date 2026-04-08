#include "service_registry.h"

#include <chrono>
#include <iostream>
#include <thread>

const std::string ServiceRegistry::ROOT_PATH = "/rpc_mt";

ServiceRegistry::ServiceRegistry(const std::string& zk_hosts) {
  zoo_set_debug_level(ZOO_LOG_LEVEL_WARN);
  this->zk_handle_ =
      zookeeper_init(zk_hosts.c_str(), this->GlobalWatcher, 30000, 0, this, 0);
  if (zk_handle_ == nullptr) {
    throw std::runtime_error("zookeeper_init failed");
  };
  int retry = 0;
  while (!this->is_connected_ && retry < 10) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    retry++;
  }
  if (!this->is_connected_) {
    LOG_ERROR("zookeeper_init timeout");
  } else {
    LOG_INFO("zookeeper_init success , connect {}", zk_hosts);
  }
}

ServiceRegistry::~ServiceRegistry() {
  if (this->zk_handle_ != nullptr) {
    try {
      zookeeper_close(this->zk_handle_);
      LOG_INFO("zookeeper_close success");
      this->zk_handle_ = nullptr;
    } catch (std::exception& e) {
      LOG_ERROR("zookeeper_close failed: {}", e.what());
      this->zk_handle_ = nullptr;
    } catch (...) {
      LOG_ERROR("zookeeper_close unknown failed");
      this->zk_handle_ = nullptr;
    }
  }
}

bool ServiceRegistry::Register(const std::string& service_name,
                               const std::string& service_addr) {
  if (!this->IsConnected()) {
    LOG_ERROR("Register failed: not connected");
    return false;
  }
  const std::string& path = this->ROOT_PATH + "/" + service_name;
  if (!this->EnSurePath(path)) {
    LOG_ERROR("Register {} failed", path);
    return false;
  };
  const std::string& temp_path = path + "/" + service_addr;
  if (!this->CreateNode(temp_path, service_addr, ZOO_EPHEMERAL)) {
    LOG_ERROR("Register temporary node failed");
    return false;
  };
  LOG_INFO("Register success , service_name: {}, service_addr: {}",
           service_name, service_addr);
  return true;
}

bool ServiceRegistry::IsConnected() const {
  return this->is_connected_ && this->zk_handle_ != nullptr;
}

void ServiceRegistry::GlobalWatcher(zhandle_t* zk_handle, int type, int state,
                                    const char* path, void* watcher_ctx) {
  if (watcher_ctx == nullptr) {
    return;
  }
  ServiceRegistry* registry = static_cast<ServiceRegistry*>(watcher_ctx);
  if (type == ZOO_SESSION_EVENT) {
    if (state == ZOO_CONNECTED_STATE) {
      registry->is_connected_ = true;
      LOG_INFO("zookeeper connected");
    } else if (state == ZOO_EXPIRED_SESSION_STATE) {
      registry->is_connected_ = false;
      LOG_ERROR("zookeeper session expired");
    } else if (state == ZOO_AUTH_FAILED_STATE) {
      registry->is_connected_ = false;
      LOG_ERROR("zookeeper auth failed");
    } else {
      registry->is_connected_ = false;
      LOG_WARN("zookeeper unknown state {}", state);
    }
  }
}

bool ServiceRegistry::EnSurePath(const std::string& path) {
  if (path.empty()) {
    LOG_ERROR("EnSurePath path is empty");
    return false;
  };
  if (this->zk_handle_ == nullptr) {
    LOG_ERROR("EnSurePath zk_handle_ is nullptr");
    return false;
  };
  auto pos = path.find_last_of('/');
  if (pos != std::string::npos && pos != 0) {
    auto parent_path = path.substr(0, pos);
    if (parent_path != "" && !this->EnSurePath(parent_path)) {
      LOG_ERROR("EnSurePath {} failed", parent_path);
      return false;
    };
  };
  struct Stat stat;
  int ret = zoo_exists(zk_handle_, path.c_str(), 0, &stat);
  if (ret == ZOK) {
    LOG_DEBUG("EnSurePath {} exists", path);
    return true;
  } else if (ret == ZNONODE) {
    ret = zoo_create(this->zk_handle_, path.c_str(), "", 0,
                     &ZOO_OPEN_ACL_UNSAFE, 0, nullptr, 0);
    if (ret == ZOK) {
      LOG_INFO("EnSurePath {} success", path);
      return true;
    } else if (ret == ZNODEEXISTS) {
      LOG_INFO("EnSurePath {} exists", path);
      return true;
    } else {
      LOG_ERROR("EnSurePath {} failed: {}", path, ret);
      return false;
    }
  } else {
    LOG_ERROR("EnSurePath {} failed: {}", path, ret);
    return false;
  }
}

bool ServiceRegistry::CreateNode(const std::string& path,
                                 const std::string& value, int flags) {
  if (this->zk_handle_ == nullptr) {
    LOG_ERROR("CreateNode zk_handle_ is nullptr");
    return false;
  };
  int ret = zoo_create(this->zk_handle_, path.c_str(), value.c_str(),
                       value.size(), &ZOO_OPEN_ACL_UNSAFE, flags, nullptr, 0);
  if (ret == ZOK) {
    LOG_INFO("CreateNode {} success", path);
    return true;
  } else if (ret == ZNODEEXISTS) {
    LOG_INFO("CreateNode {} exists", path);
    return true;
  } else {
    LOG_ERROR("CreateNode {} failed: {}", path, ret);
    return false;
  }
}
