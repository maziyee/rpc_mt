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
  // ⚠️ 不能直接遍历 m_connects。Close() 会触发 close_callback_，那条链一路
  //    走到 RemoveConnect() —— 也就是【一边遍历一边删正在遍历的 map】，
  //    迭代器失效。而且原来这里压根没加锁，别的线程 AddConnect 同样能搞坏它。
  //
  // 先 swap 到局部变量（O(1)，只换指针、不拷贝元素），m_connects 变成空的。
  // 之后回调里的 RemoveConnect 操作的是那个已经空的 map，是无害的 no-op；
  // 而遍历的是私有容器，别的线程碰不到。
  //
  // 同一个手法在 MysqlClient::Close() 里也有（那里是为了到锁外去关连接）。
  std::unordered_map<int, std::shared_ptr<Connect>> conns;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    conns.swap(this->m_connects);
  }
  for (auto& kv : conns) {
    kv.second->Close();
  }
  LOG_INFO("ConnectManage::CloseAll: {} connections closed", conns.size());
}
