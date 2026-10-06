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

void rpc::ManagerCycle::RequestStop() { is_stop_ = true; }

void rpc::ManagerCycle::Stop() {
  this->is_running_ = false;
  is_stop_ = true;
  for (auto& it : listen_fds_) {
    it.second->Close();
  }
  this->connect_manage_->CloseAll();
}

void rpc::ManagerCycle::HandleEvent(int fd, uint32_t events) {
  // EPOLLRDHUP 也列在这里：Add() 一直注册着它，但从没有人检查过 ——
  // 对端半关闭时只有它报，靠 recv()==0 兜底并不总是成立。
  if (events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
    LOG_ERROR("HandleEvent error: fd={} events={:#x}", fd, events);
    // ⚠️ 走 RemoveConnect，不是 Remove。Remove 只把 fd 从 epoll 摘掉，
    //    连接对象会留在 ConnectManage 里、fd 也不关 —— 慢慢泄漏。
    //    RemoveConnect 是两者都做（对照它的定义）。
    this->RemoveConnect(fd);
    return;
  }
  if (listen_fds_.find(fd) != listen_fds_.end()) {
    LOG_INFO("HandleEvent listen fd: {}", fd);
    Socket* socket = listen_fds_[fd];
    this->HandleNewConnection(socket);
  } else {
    if (events & EPOLLIN) {
      LOG_INFO("HandleEvent read fd: {}", fd);
      this->HandleData(fd);
    }
    // Add() 注册了 EPOLLOUT，但这里原来没有对应分支 —— 事件被直接丢弃。
    //
    // ET 模式下 EPOLLOUT 只在"发送缓冲由满转不满"时报【一次】，而那正是重发
    // 残留字节的唯一时机。不处理它，SentBufInfo 里因 EAGAIN 留下的那截数据
    // 就永远发不出去：客户端表现为"卡住不报错"，服务端日志一片安静。
    // 响应路径还能靠"下一次 Write 顺带重发"糊过去，推送没有下一次。
    if (events & EPOLLOUT) {
      this->HandleWrite(fd);
    }
  }
}

void rpc::ManagerCycle::HandleWrite(int fd) {
  auto conn = this->connect_manage_->GetConnect(fd);
  if (!conn || !conn->IsRunning()) {
    return;  // 可能刚被 HandleData 摘掉，静默返回
  }
  // 绝大多数 EPOLLOUT 都在这一行返回：无锁读一个 atomic，不必为了确认
  // "没东西要发"去抢 write_mutex_。
  if (!conn->HasPendingSend()) {
    return;
  }
  if (!conn->FlushSendBuf()) {
    LOG_ERROR("HandleWrite flush failed, fd={}", fd);
    this->RemoveConnect(fd);
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
  connect->SetRequestCallback(
      [this](const std::shared_ptr<Connect>& conn, const RpcRequest& request) {
        this->HandleMessage(conn, request);
      });
  connect->SetCloseCallback([this](const std::shared_ptr<Connect>& conn) {
    this->HandleClose(conn);
  });
  this->connect_manage_->AddConnect(connect);
  Add(fd, EPOLLIN | EPOLLOUT | EPOLLRDHUP);
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
  LOG_INFO("HandleData read success: {} , recv_buf_size: {} ", fd,
           conn->GetReadBuf().size());
  if (!conn->ProgressGetMessage()) {
    LOG_ERROR("HandleData ProgressGetMessage error: {}", fd);
    this->RemoveConnect(fd);
    return;
  }
}

void rpc::ManagerCycle::RemoveConnect(int fd) {
  this->Remove(fd);  // 先从 epoll 移除（fd 此时仍有效）
  this->connect_manage_->RemoveConnect(fd);  // 再释放连接（引用计数归零时析构关 fd）
}

void rpc::ManagerCycle::HandleMessage(const std::shared_ptr<Connect>& connect,
                                      const rpc::RpcRequest& request) {
  this->HandleMessageAsync(connect, request);
}

void rpc::ManagerCycle::HandleClose(const std::shared_ptr<Connect>& connect) {
  this->connect_manage_->RemoveConnect(connect->GetFd());
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

bool rpc::ManagerCycle::AddListenFd(int fd, uint32_t events, Socket* socket) {
  struct epoll_event event;
  event.events = events | EPOLLET;
  event.data.fd = fd;
  int ret = epoll_ctl(this->epoll_fd_, EPOLL_CTL_ADD, fd, &event);
  if (ret == -1) {
    LOG_ERROR("epoll_ctl_add failed: {}", strerror(errno));
    return false;
  }
  LOG_INFO("epoll_ctl_add success: {}", fd);
  this->listen_fds_[fd] = socket;
  return true;
}

void rpc::ManagerCycle::Add(int fd, uint32_t events) {
  struct epoll_event event;
  event.events = events | EPOLLET;
  event.data.fd = fd;
  int ret = epoll_ctl(this->epoll_fd_, EPOLL_CTL_ADD, fd, &event);
  if (ret == -1) {
    LOG_ERROR("epoll_ctl_add failed: {}", strerror(errno));
    return;
  }
  LOG_INFO("epoll_ctl_add success: {}", fd);
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
  LOG_INFO("HandleMessageAsync start");
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
