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

rpc::Connect::~Connect() {
  this->is_running_.store(false);
  if (this->fd_ > 0) {
    ::close(this->fd_);
    LOG_INFO("Connect::~Connect close fd: {}", this->fd_);
    this->fd_ = -1;
  }
  this->recv_buf_.clear();
  this->send_buf_.clear();
}

bool rpc::Connect::Read() {
  std::string recv = this->ReadTheInfo();
  if (!this->IsRunning()) {
    return false;
  }
  if (!recv.empty()) {
    this->recv_buf_.insert(recv_buf_.end(), recv.begin(), recv.end());
    LOG_INFO("the recv info size: {}", recv.size());
  }
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
  std::lock_guard<std::mutex> lock(write_mutex_);
  this->send_buf_.insert(send_buf_.end(), encrypt.begin(), encrypt.end());
  LOG_INFO("sent info to the buf");
  return this->SentBufInfo();
}

bool rpc::Connect::Write(RpcRequest& request) {
  std::string serialized;
  if (!request.Serializer(serialized)) {
    LOG_ERROR("SendReq error in Serializer");
    return false;
  };
  std::string compress_data;
  if (!rpc::ZstdCompress::GetInstance().CompressString(serialized,
                                                       compress_data)) {
    LOG_ERROR("SendReq error in CompressString");
    return false;
  };
  std::string encrypt;
  if (!rpc::AesEncrypt::GetInstance().Encrypt(compress_data, encrypt)) {
    LOG_ERROR("SendReq error in Encrypt");
    return false;
  }
  std::lock_guard<std::mutex> lock(write_mutex_);
  this->send_buf_.insert(send_buf_.end(), encrypt.begin(), encrypt.end());
  LOG_INFO("sent info to the buf");
  return this->SentBufInfo();
}

void rpc::Connect::Close() {
  if (!this->is_running_.load()) {
    return;
  }
  this->is_running_.store(false);
  if (this->close_callback_) {
    this->close_callback_(shared_from_this());
  }
  // fd 由析构函数关闭，确保 epoll 先完成 DEL 再关 fd
  this->recv_buf_.clear();
  {
    std::lock_guard<std::mutex> lock(write_mutex_);
    this->send_buf_.clear();
  }
}

bool rpc::Connect::SentBufInfo() {
  if (this->send_buf_.empty()) {
    LOG_INFO("the send buf is empty");
    return true;
  }
  int total_send = 0;
  while (total_send < static_cast<int>(this->send_buf_.size())) {
    int send_size = ::send(this->fd_, this->send_buf_.data() + total_send,
                           this->send_buf_.size() - total_send, 0);
    if (send_size < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      LOG_ERROR("send error: {}", strerror(errno));
      return false;
    } else if (send_size == 0) {
      LOG_WARN("send 0 bytes, peer closed, fd: {}", this->fd_);
      return false;
    }
    total_send += send_size;
  }
  this->send_buf_.erase(send_buf_.begin(), send_buf_.begin() + total_send);
  return true;
}

bool rpc::Connect::ProgressGetMessage() {
  if (this->recv_buf_.size() >= sizeof(RpcHeader)) {
    LOG_INFO("the recv buf size: {}", this->recv_buf_.size());
    try {
      std::string encrypt_string(this->recv_buf_.begin(),
                                 this->recv_buf_.end());
      std::string decrypt_string;
      if (encrypt_string.empty()) {
        LOG_ERROR("the encrypt_string is empty");
        return false;
      }
      if (!rpc::AesEncrypt::GetInstance().Decrypt(encrypt_string,
                                                  decrypt_string)) {
        LOG_ERROR("Decrypt error");
        return false;
      }
      LOG_INFO("the decrypt_string size: {}", decrypt_string.size());
      std::string decompress_data;
      if (!rpc::ZstdCompress::GetInstance().DecompressString(decrypt_string,
                                                             decompress_data)) {
        LOG_ERROR("DecompressString error");
        return false;
      }
      LOG_INFO("the decompress_data size: {}", decompress_data.size());
      if (decompress_data.size() < sizeof(RpcHeader)) {
        LOG_DEBUG("waiting for more data");
        return true;
      }
      RpcHeader header;
      std::memcpy(&header, decompress_data.data(), sizeof(RpcHeader));
      if (header.magic_ != RpcHeader::kMagic) {
        LOG_ERROR("the magic is not match");
        return false;
      }
      uint32_t total_size = header.body_size_ + sizeof(RpcHeader);
      if (decompress_data.size() < total_size) {
        LOG_DEBUG("waiting for more data");
        return true;
      }
      LOG_INFO("the total_size: {}", total_size);
      RpcRequest request;
      if (!request.Deserializer(decompress_data)) {
        LOG_ERROR("Deserializer error");
        return false;
      }
      LOG_INFO("the request get success");
      if (this->message_callback_) {
        message_callback_(shared_from_this(), request);
      }
      this->recv_buf_.clear();
      LOG_INFO("the request info process success");
      return true;
    } catch (std::exception& e) {
      LOG_ERROR("ProgressGetMessage error: {}", e.what());
      return false;
    }
  }
  return true;
}

bool rpc::Connect::ReadWithTimeout(int timeout_ms) {
  while (true) {
    fd_set read_set;
    struct timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    FD_ZERO(&read_set);
    FD_SET(this->fd_, &read_set);
    int ret = ::select(this->fd_ + 1, &read_set, NULL, NULL, &timeout);
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      LOG_ERROR("select error: {}", strerror(errno));
      return false;
    }
    if (ret == 0) {
      LOG_ERROR("select timeout");
      return false;
    }
    if (FD_ISSET(this->fd_, &read_set)) {
      return this->Read();
    }
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
