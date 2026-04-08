#include "connect_manage.h"

rpc::ConnectManage::~ConnectManage() {
  LOG_INFO("ConnectManage::~ConnectManage");
  this->CloseAll();
}

bool rpc::ConnectManage::AddConnect(std::shared_ptr<Connect> connect) {
  if (!connect) {
    LOG_ERROR("connect is null");
    return false;
  }
  int fd = connect->GetFd();
  if (fd < 0) {
    LOG_ERROR("fd is invalid");
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (this->m_connects.find(fd) != this->m_connects.end()) {
      return false;
    }
    this->m_connects[fd] = connect;
  }
  LOG_INFO("ConnectManage::AddConnect fd:%d", fd);
  return true;
}

bool rpc::ConnectManage::RemoveConnect(int fd) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (this->m_connects.find(fd) != this->m_connects.end()) {
    this->m_connects.erase(fd);
    return true;
  }
  return false;
}

std::shared_ptr<rpc::Connect> rpc::ConnectManage::GetConnect(int fd) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (this->m_connects.find(fd) != this->m_connects.end()) {
    return this->m_connects[fd];
  }
  LOG_ERROR("ConnectManage::GetConnect fd:%d not found");
  return nullptr;
}

void rpc::ConnectManage::CloseAll() {
  for (auto& it : this->m_connects) {
    it.second->Close();
  }
  LOG_INFO("ConnectManage::ClossAll");
  this->m_connects.clear();
}
