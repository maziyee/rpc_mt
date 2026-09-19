#include "service_registry.h"

namespace rpc {

const std::string ServiceRegistry::ROOT_PATH = "/rpc_mt";

ServiceRegistry::ServiceRegistry(ZkHandle zk_handle)
    : zk_handle_(std::move(zk_handle)) {
  if (this->zk_handle_ == nullptr) {
    throw std::runtime_error("ServiceRegistry: null zk handle");
  }
}

// 不再维护自己的一份连接状态 —— 那个 is_connected_ 标志既依赖 watcher 回调
// 时机，又是跨线程读写的裸 bool。直接同步查 zoo_state()，语义等价且更准。
bool ServiceRegistry::IsConnected() const {
  return this->zk_handle_ != nullptr &&
         zoo_state(this->zk_handle_.get()) == ZOO_CONNECTED_STATE;
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
  int ret = zoo_exists(zk_handle_.get(), path.c_str(), 0, &stat);
  if (ret == ZOK) {
    LOG_DEBUG("EnSurePath {} exists", path);
    return true;
  } else if (ret == ZNONODE) {
    ret = zoo_create(this->zk_handle_.get(), path.c_str(), "", 0,
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
  int ret = zoo_create(this->zk_handle_.get(), path.c_str(), value.c_str(),
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

}  // namespace rpc
