#pragma once

#include <sys/types.h>

#include <string>

class Socket {
 public:
  Socket(const std::string& ip, const int port, const int max_connection_count,
         const double socket_timeout);
  ~Socket();
  Socket(const Socket& other) = delete;
  Socket& operator=(const Socket& other) = delete;

  bool SetSocketOption();

  bool SetClientSocketOption(int client_fd);

  int GetFd() const { return fd_; };

  int GetPort() const { return socket_port_; };

  int GetSocketTimeout() const { return socket_timeout_; };

  int Accept();
  bool Close();

 private:
  void Init();
  void Create();

  void Bind();

  void Listen();

  void SetNotBlocking(int fd);

 private:
  int fd_;
  double socket_timeout_;
  uint16_t socket_port_;
  int max_connection_count_;
  std::string ip_;
};
