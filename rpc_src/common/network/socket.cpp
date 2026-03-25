#include "socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log_manager.h"

Socket::Socket(const std::string& ip, const int port,
               const int max_connection_count, const double socket_timeout)
    : ip_(ip),
      socket_port_(port),
      max_connection_count_(max_connection_count),
      socket_timeout_(socket_timeout) {}

bool Socket::SetSocketOption() {
  struct timeval send_timeout;
  send_timeout.tv_sec = static_cast<int>(this->socket_timeout_);
  send_timeout.tv_usec =
      static_cast<int>((this->socket_timeout_ - send_timeout.tv_sec) * 1000000);
  if (::setsockopt(this->fd_, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
                   sizeof(send_timeout)) < 0) {
    LOG_ERROR("Socket fd set send timeout failed");
    return false;
  }
  struct timeval recv_timeout;
  recv_timeout.tv_sec = static_cast<int>(this->socket_timeout_);
  recv_timeout.tv_usec =
      static_cast<int>((this->socket_timeout_ - recv_timeout.tv_sec) * 1000000);
  if (::setsockopt(this->fd_, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout,
                   sizeof(recv_timeout)) < 0) {
    LOG_ERROR("Socket fd set recv timeout failed");
    return false;
  }

  int reuse = 1;
  if (::setsockopt(this->fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) <
      0) {
    LOG_ERROR("Socket fd set reuse addr failed");
    return false;
  }

  if (::setsockopt(this->fd_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) <
      0) {
    LOG_ERROR("Socket fd set reuse port failed");
    return false;
  }

  int keep_alive = 1;
  if (::setsockopt(this->fd_, SOL_SOCKET, SO_KEEPALIVE, &keep_alive,
                   sizeof(keep_alive)) < 0) {
    LOG_ERROR("Socket fd set keep alive failed");
    return false;
  }

  int nodelay = 1;
  if (::setsockopt(this->fd_, IPPROTO_TCP, TCP_NODELAY, &nodelay,
                   sizeof(nodelay)) < 0) {
    LOG_ERROR("Socket fd set no delay failed");
    return false;
  }
  this->SetNotBlocking(this->fd_);
  return true;
}

bool Socket::SetClientSocketOption(int client_fd) {
  int keep_alive = 1;
  if (::setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &keep_alive,
                   sizeof(keep_alive)) < 0) {
    LOG_ERROR("Socket fd set keep alive failed");
    return false;
  }
  int nodelay = 1;
  if (::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay,
                   sizeof(nodelay)) < 0) {
    LOG_ERROR("Socket fd set no delay failed");
    return false;
  }
  this->SetNotBlocking(client_fd);
  return true;
}

int Socket::Accept() {
  struct sockaddr_in client_addr;
  socklen_t client_addr_len = sizeof(client_addr);
  memset(&client_addr, 0, sizeof(client_addr));
  int client_fd =
      ::accept(this->fd_, (struct sockaddr*)&client_addr, &client_addr_len);
  if (client_fd < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return -1;
    }
    LOG_ERROR("Socket fd accept failed");
    return -1;
  }
  LOG_INFO("Socket fd accepted");

  if (!this->SetClientSocketOption(client_fd)) {
    ::close(client_fd);
    LOG_ERROR("Socket fd set client socket option failed");
    return -1;
  };
  return client_fd;
}

void Socket::Init() {
  this->Create();
  this->Bind();
  this->Listen();
  this->SetSocketOption();
}

void Socket::Create() {
  this->fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (this->fd_ < 0) {
    LOG_ERROR("Socket fd create failed");
    throw std::runtime_error("Socket fd create failed");
  }
  LOG_INFO("Socket fd created");
}

bool Socket::Close() {
  if (::close(this->fd_) < 0) {
    LOG_ERROR("Socket fd close failed");
    return false;
  }
  fd_ = -1;
  LOG_INFO("Socket fd closed");
  return true;
}

void Socket::Bind() {
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(this->socket_port_);
  addr.sin_addr.s_addr = INADDR_ANY;
  if (::bind(this->fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    LOG_ERROR("Socket fd bind failed");
    this->Close();
    throw std::runtime_error("Socket fd bind failed");
  }
  LOG_INFO("Socket fd binded");
}

void Socket::Listen() {
  if (::listen(this->fd_, this->max_connection_count_) < 0) {
    LOG_ERROR("Socket fd listen failed");
    this->Close();
    throw std::runtime_error("Socket fd listen failed");
  }
  LOG_INFO("Socket fd listened");
}

void Socket::SetNotBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  flags |= O_NONBLOCK;
  if (fcntl(fd, F_SETFL, flags) < 0) {
    LOG_ERROR("Socket fd set nonblocking failed");
    ::close(fd);
    throw std::runtime_error("Socket fd set nonblocking failed");
  }
}
