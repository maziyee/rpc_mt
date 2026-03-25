#include "connect.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

#include "aes_encrypt.h"
#include "connect_manage.h"
#include "log_manager.h"
#include "service_manager.h"
#include "socket.h"
#include "thread_single.h"
#include "zstd_compress.h"

rpc::Connect::Connect(int fd) : fd_(fd) {
  if (fd_ < 0) {
    LOG_ERROR("the fd is invalid");
    return;
  }
  int type;
  socklen_t len = sizeof(type);
  if (::getsockopt(fd_, SOL_SOCKET, SO_TYPE, &type, &len) < 0) {
    LOG_ERROR("getsockopt error: {}", strerror(errno));
    this->is_running_.store(false);
    return;
  }
  struct sockaddr_in addr;
  socklen_t addr_len = sizeof(addr);
  if (::getpeername(fd_, (struct sockaddr*)&addr, &addr_len) == 0) {
    char ip[INET_ADDRSTRLEN];
    ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    LOG_INFO("the client ip: {}", ip);
    this->ip_ = ip;
    this->port_ = ntohs(addr.sin_port);
    this->is_running_.store(true);
    LOG_INFO("the client port: {}", this->port_);
  } else {
    LOG_ERROR("getpeername error: {}", strerror(errno));
    this->is_running_.store(false);
    return;
  }
}

bool rpc::Connect::Read() {
  std::string recv = this->ReadTheInfo();
  if (recv.empty()) {
    LOG_ERROR("the recv is empty");
    return false;
  }
  this->recv_buf_.insert(recv_buf_.end(), recv.begin(), recv.end());
  LOG_INFO("the recv info: {}", recv);
  return true;
}

bool rpc::Connect::Write(RpcResponse& response) {
  std::string serialized;
  if (!response.Serializer(serialized)) {
    LOG_ERROR("SendRes error in Serializer");
    return false;
  };
  std::string compress_data;
  if (!rpc::ZstdCompress::GetInstance().CompressString(serialized,
                                                       compress_data)) {
    LOG_ERROR("SendRes error in CompressString");
    return false;
  };
  std::string encrypt;
  if (!rpc::AesEncrypt::GetInstance().Encrypt(compress_data, encrypt)) {
    LOG_ERROR("SendRes error in Encrypt");
    return false;
  }
  this->recv_buf_.insert(send_buf_.begin(), encrypt.begin(), encrypt.end());
  LOG_INFO("sent info to the buf");
  return this->SentBufInfo();
}

void rpc::Connect::Close() {
  if (!this->is_running_.load()) {
    return;
  }
  this->is_running_.store(false);
  if (this->fd_ > 0) {
    ::close(this->fd_);
    this->fd_ = -1;
    LOG_INFO("Connect::Close fd: {}", this->fd_);
  }
  this->recv_buf_.clear();
  this->send_buf_.clear();
  if (this->close_callback_) {
    this->close_callback_(shared_from_this());
  }
}

bool rpc::Connect::SentBufInfo() {
  if (this->send_buf_.empty()) {
    return true;
  }
  int total_send = 0;
  while (total_send <= this->send_buf_.size()) {
    int send_size = ::send(this->fd_, this->send_buf_.data() + total_send,
                           this->send_buf_.size() - total_send, 0);
    if (send_size < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return true;
      }
      LOG_ERROR("send error: %s", strerror(errno));
      this->Close();
      return false;
    } else if (send_size == 0) {
      LOG_WARN("send 0 info , {} ", this->fd_);
      break;
    }
    total_send += send_size;
  }
  this->send_buf_.erase(send_buf_.begin(), send_buf_.begin() + total_send);
  return true;
}

bool rpc::Connect::ProgressGetMessage() {
  RpcHeader header;
  if (this->recv_buf_.size() < sizeof(header)) {
    return true;
  }
  memcpy(&header, this->recv_buf_.data(), sizeof(header));
  if (header.kMagic != RpcHeader::kMagic) {
    LOG_ERROR("ProgressGetMessage error: magic number is not valid");
    return false;
  }
  if (this->recv_buf_.size() < header.body_size_ + sizeof(header)) {
    return true;
  }
  std::string get_message(this->recv_buf_.begin(),
                          this->recv_buf_.begin() + header.body_size_);
  this->recv_buf_.erase(recv_buf_.begin(),
                        recv_buf_.begin() + header.body_size_ + sizeof(header));

  try {
    std::string decrypt_data;
    if (!rpc::AesEncrypt::GetInstance().Decrypt(get_message, decrypt_data)) {
      LOG_ERROR("ProgressGetMessage error: decrypt data failed");
      return false;
    }
    std::string decompress_data;
    if (!rpc::ZstdCompress::GetInstance().DecompressString(decrypt_data,
                                                           decompress_data)) {
      LOG_ERROR("ProgressGetMessage error: decompress data failed");
      return false;
    }
    RpcRequest request;
    if (!request.Deserializer(decompress_data)) {
      LOG_ERROR("ProgressGetMessage error: deserialize data failed");
      return false;
    }
    if (this->message_callback_) {
      this->message_callback_(shared_from_this(), request);
    }
    if (this->recv_buf_.size() > sizeof(header)) {
      return this->ProgressGetMessage();
    }
    return true;
  } catch (std::exception& e) {
    LOG_ERROR("ProgressGetMessage error: {}", e.what());
    return false;
  }
}

std::string rpc::Connect::ReadTheInfo() {
  if (!this->IsRunning()) {
    LOG_ERROR("the connect is invalid");
    return "";
  }
  char buf[4096];
  std::string res;
  while (true) {
    ssize_t recv_nums = ::recv(this->fd_, buf, sizeof(buf), 0);
    if (recv_nums > 0) {
      res.append(buf, recv_nums);
      LOG_INFO("recv info: {}", res);
    } else if (recv_nums == 0) {
      LOG_INFO("recv didn't get info");
      this->Close();
      break;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      LOG_ERROR("recv error: {}", strerror(errno));
      this->Close();
      break;
    }
  }
  return res;
}
