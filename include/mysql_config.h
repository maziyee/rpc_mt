#pragma once

#include <string>

namespace rpc {

// MySQL 连接配置。
// zk_retry_times 从没被读过），所以：
//   加一个键 = 同时加读取它的代码，且键名两边逐字一致
class MysqlConfig {
 public:
  MysqlConfig() = default;
  ~MysqlConfig() = default;

  bool Init(const std::string& config_path);

  // 而不是 TCP，会连到编译期定死的 socket 路径。写 IP 才是可预测的 TCP 连接。
  std::string GetHost() const { return host_; }
  int GetPort() const { return port_; }
  std::string GetUser() const { return user_; }
  std::string GetPassword() const { return password_; }
  std::string GetDatabase() const { return database_; }
  int GetConnectTimeoutSec() const { return connect_timeout_sec_; }
  int GetPoolSize() const { return pool_size_; }

  // 供启动日志使用：输出连接信息，但【密码必须打码】。
  // 日志泄露凭据是很常见的事故，所以打印配置这件事要专门处理。
  std::string Describe() const;

  // 配置对象不复制、不移动：加载完就固定下来，只读使用
  MysqlConfig(const MysqlConfig&) = delete;
  MysqlConfig& operator=(const MysqlConfig&) = delete;
  MysqlConfig(MysqlConfig&&) = delete;
  MysqlConfig& operator=(MysqlConfig&&) = delete;

 private:
  // setter 私有：只有 Init 能改，外部只能读 —— 保证"配置一旦加载不再变"
  void SetHost(const std::string& host) { host_ = host; }
  void SetPort(int port) { port_ = port; }
  void SetUser(const std::string& user) { user_ = user; }
  void SetPassword(const std::string& password) { password_ = password; }
  void SetDatabase(const std::string& database) { database_ = database; }
  void SetConnectTimeoutSec(int sec) { connect_timeout_sec_ = sec; }
  void SetPoolSize(int size) { pool_size_ = size; }

 private:
  // 成员都带默认值：若 Init 失败或未被调用，getter 返回的是确定的默认值，
  // 而不是未初始化的随机内存（int 成员不初始化时读它是 UB）
  std::string host_ = "127.0.0.1";
  int port_ = 3306;
  std::string user_;
  std::string password_;
  std::string database_ = "rpc_mt";
  int connect_timeout_sec_ = 3;
  int pool_size_ = 4;
};

}  // namespace rpc
