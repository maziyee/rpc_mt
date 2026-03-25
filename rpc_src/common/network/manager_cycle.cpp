#include "manager_cycle.h"

#include <signal.h>

#include <atomic>

namespace rpc {

static std::atomic<bool> is_stop_ = false;
static void sigHandle(int sig) { is_stop_ = true; }
}  // namespace rpc
rpc::ManagerCycle::ManagerCycle(ConnectManage* manager)
    : connect_manage_(manager), is_running_(false) {
  this->Create();
}

rpc::ManagerCycle::~ManagerCycle() {
  LOG_INFO("ManagerCycle::~ManagerCycle");
  this->Stop();
}

void rpc::ManagerCycle::Loop() {
  this->is_running_ = true;
  while (!is_stop_ && this->is_running_) {
    auto events = this->Wait(1000);
    if (events.empty()) {
      continue;
    }
    for (auto& event : events) {
      this->HandleEvent(event.data.fd, event.events);
    }
  }
  this->is_running_ = false;
}

void rpc::ManagerCycle::Stop() {
  this->is_running_ = false;
  is_stop_ = true;
  for (auto& it : listen_fds_) {
    it.second->Close();
  }
  this->connect_manage_->ClossAll();
}

void rpc::ManagerCycle::HandleEvent(int fd, uint32_t events) {
  if (events & EPOLLHUP | EPOLLERR) {
    LOG_ERROR("HandleEvent error: {}", fd);
    this->Remove(fd);
    return;
  }
  if (listen_fds_.find(fd) != listen_fds_.end()) {
    LOG_INFO("HandleEvent listen fd: {}", fd);
    Socket* socket = listen_fds_[fd];
    this->HandleNewConnection(socket);
  } else {
    LOG_INFO("HandleEvent other fd: {}", fd);
    this->HandleData(fd);
  }
}

void rpc::ManagerCycle::HandleNewConnection(Socket* socket) {
  int fd = socket->Accept();
  if (fd < 0) {
    LOG_ERROR("HandleNewConnection error: {}", fd);
    return;
  }
  LOG_INFO("HandleNewConnection success: {}", fd);
  std::shared_ptr<Connect> connect = std::make_shared<Connect>(fd);
  connect->SetMessageCallback(
      [this](const std::shared_ptr<Connect>& conn, const RpcRequest& request) {
        this->HandleMessage(conn, request);
      });
  connect->SetCloseCallback([this](const std::shared_ptr<Connect>& conn) {
    this->HandleClose(conn);
  });
  this->connect_manage_->AddConnect(connect);
  this->AddListenFd(fd, EPOLLIN | EPOLLOUT | EPOLLRDHUP);
}

void rpc::ManagerCycle::HandleData(int fd) {
  auto conn = this->connect_manage_->GetConnect(fd);
  if (!conn) {
    LOG_ERROR("HandleData error: {}", fd);
    this->RemoveConnect(fd);
    return;
  }
  if (!conn->IsRunning()) {
    LOG_ERROR("HandleData error: {}", fd);
    this->RemoveConnect(fd);
    return;
  };

  if (!conn->Read()) {
    LOG_ERROR("HandleData read error: {}", fd);
    this->RemoveConnect(fd);
    return;
  }

  if (!conn->ProgressGetMessage()) {
    LOG_ERROR("HandleData ProgressGetMessage error: {}", fd);
    this->RemoveConnect(fd);
    return;
  }
}

void rpc::ManagerCycle::RemoveConnect(int fd) {
  this->connect_manage_->RemoveConnect(fd);
  this->Remove(fd);
}

void rpc::ManagerCycle::HandleMessage(const std::shared_ptr<Connect>& connect,
                                      const rpc::RpcRequest& request) {
  this->HandleMessageAsync(connect, request);
}

void rpc::ManagerCycle::Create() {
  this->epoll_fd_ = epoll_create1(0);
  if (this->epoll_fd_ == -1) {
    LOG_ERROR("epoll_create1 failed: {}", strerror(errno));
    throw std::runtime_error("epoll_create1 failed");
  }

  LOG_INFO("epoll_create1 success: {}", this->epoll_fd_);

  if (::signal(SIGINT, sigHandle) == SIG_ERR) {
    LOG_ERROR("signal register failed: {}", strerror(errno));
    throw std::runtime_error("signal failed");
  }
}

bool rpc::ManagerCycle::AddListenFd(int fd, uint32_t events) {
  struct epoll_event event;
  event.events = events | EPOLLET;
  event.data.fd = fd;
  int ret = epoll_ctl(this->epoll_fd_, EPOLL_CTL_ADD, fd, &event);
  if (ret == -1) {
    LOG_ERROR("epoll_ctl_add failed: {}", strerror(errno));
    return false;
  }
  LOG_INFO("epoll_ctl_add success: {}", fd);
  return true;
}

void rpc::ManagerCycle::Modify(int fd, uint32_t events) {
  struct epoll_event event;
  event.events = events | EPOLLET;
  event.data.fd = fd;
  int ret = epoll_ctl(this->epoll_fd_, EPOLL_CTL_MOD, fd, &event);
  if (ret == -1) {
    LOG_ERROR("epoll_ctl_modify failed: {}", strerror(errno));
    return;
  }
  LOG_INFO("epoll_ctl_modify success: {}", fd);
}

void rpc::ManagerCycle::Remove(int fd) {
  int ret = epoll_ctl(this->epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  if (ret == -1) {
    LOG_ERROR("epoll_ctl_remove failed: {}", strerror(errno));
    return;
  }
  LOG_INFO("epoll_ctl_remove success: {}", fd);
}

std::vector<struct epoll_event> rpc::ManagerCycle::Wait(int timeout_ms) {
  std::vector<struct epoll_event> events(this->kMAX_EVENTS);
  int nfds =
      epoll_wait(this->epoll_fd_, events.data(), events.size(), timeout_ms);
  if (nfds == -1) {
    LOG_ERROR("epoll_wait failed: {}", strerror(errno));
    return {};
  }
  events.resize(nfds);
  return events;
}

void rpc::ManagerCycle::HandleMessageAsync(
    const std::shared_ptr<Connect>& connect, const rpc::RpcRequest& request) {
  if (!(meeting_ctrl::ThreadSingle::GetInstance().GetState() ==
        meeting_ctrl::ThreadStatus::kRunning)) {
    LOG_ERROR("HandleMessageAsync error: {}", connect->GetFd());
    return;
  }
  auto future = meeting_ctrl::ThreadSingle::GetInstance().Enqueue(
      meeting_ctrl::TaskPriority::kHIGH, [this, connect, request]() {
        this->HandleMessageSync(connect, request);
      });
  if (!future.valid()) {
    LOG_ERROR("HandleMessageAsync error: {}", connect->GetFd());
    this->SendErrorRes(connect, request.GetSequenceId(), -1,
                       "HandleMessageAsync error");
    return;
  }
}

void rpc::ManagerCycle::HandleMessageSync(
    const std::shared_ptr<Connect>& connect, const rpc::RpcRequest& request) {
  if (!request.IsValid()) {
    LOG_ERROR("HandleMessageSync error: {}", connect->GetFd());
    this->SendErrorRes(connect, request.GetSequenceId(), -1,
                       "HandleMessageSync error");
    return;
  }
  std::string result;
  bool success = rpc::ServiceManager::GetInstance().HandleRequest(
      request.GetServiceName(), request.GetMethodName(), request.GetPayload(),
      result);
  if (!success) {
    LOG_ERROR("HandleMessageSync error: {}", connect->GetFd());
    this->SendErrorRes(connect, request.GetSequenceId(), -1,
                       "HandleMessageSync error");
    return;
  }
  this->SendSuccessRes(connect, request.GetSequenceId(), result);
}

bool rpc::ManagerCycle::SendErrorRes(const std::shared_ptr<Connect>& connect,
                                     int sequenceid, int error_code,
                                     std::string error_message) {
  rpc::RpcResponse response;
  response.SetSequenceId(sequenceid);
  response.SetErrorCode(error_code);
  response.SetErrorMessage(error_message);
  return this->SendRes(connect, response);
}

bool rpc::ManagerCycle::SendSuccessRes(const std::shared_ptr<Connect>& connect,
                                       int sequenceid, std::string& result) {
  rpc::RpcResponse response;
  response.SetSequenceId(sequenceid);
  response.SetResultData(result);
  response.SetErrorCode(0);
  response.SetErrorMessage("success");
  return this->SendRes(connect, response);
}

bool rpc::ManagerCycle::SendRes(const std::shared_ptr<Connect>& connect,
                                rpc::RpcResponse& response) {
  if (!connect->IsRunning()) {
    LOG_ERROR("SendRes error: {}", connect->GetFd());
    return false;
  }
  if (!connect->Write(response)) {
    LOG_ERROR("SendRes error in Send: {}", connect->GetFd());
    return false;
  }
  return true;
}
