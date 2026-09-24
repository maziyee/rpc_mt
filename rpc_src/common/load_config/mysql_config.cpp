#include "mysql_config.h"

#include <fstream>
#include <nlohmann/json.hpp>

#include "log_manager.h"

namespace rpc {
namespace {

// 从 JSON 里读一个键，键【缺失】时告警并使用默认值。
//
// 现在改成：键缺失 → 启动日志里明确 WARN 一行，一眼能发现。
//
// 注意类型错误（键在但类型不对）故意【不】吞掉：那一定是写错了，
// 让它抛出去、由 Init 的 catch 转成加载失败。缺失可以是有意的，
// 类型不对不可能是。
template <typename T>
T ReadOrWarn(const nlohmann::json& cfg, const std::string& key, const T& def) {
  if (!cfg.contains(key)) {
    LOG_WARN("mysql_config: key '{}' missing, using default", key);
    return def;
  }
  return cfg.at(key).get<T>();  // 类型不对会抛，交给外层 catch
}

}  // namespace

// 这里可以用 LOG_WARN/LOG_ERROR，而不是像 spdlog_config.cpp 那样用 std::cerr ——
// 因为 spdlog 是【最先】初始化的（见 RpcConfigManager::Init），
// 加载到本文件时日志系统已经就绪。
bool rpc::MysqlConfig::Init(const std::string& config_path) {
  try {
    std::ifstream config_file(config_path);
    if (!config_file.is_open()) {
      LOG_ERROR("mysql_config: cannot open {}", config_path);
      return false;
    }
    const nlohmann::json cfg = nlohmann::json::parse(config_file);
    config_file.close();

    this->SetHost(ReadOrWarn<std::string>(cfg, "mysql_host", this->host_));
    this->SetPort(ReadOrWarn<int>(cfg, "mysql_port", this->port_));
    this->SetUser(ReadOrWarn<std::string>(cfg, "mysql_user", this->user_));
    this->SetPassword(
        ReadOrWarn<std::string>(cfg, "mysql_password", this->password_));
    this->SetDatabase(
        ReadOrWarn<std::string>(cfg, "mysql_database", this->database_));
    this->SetConnectTimeoutSec(ReadOrWarn<int>(cfg, "mysql_connect_timeout_sec",
                                               this->connect_timeout_sec_));
    this->SetPoolSize(
        ReadOrWarn<int>(cfg, "mysql_pool_size", this->pool_size_));

    // 范围校验：非法值会让运行期出莫名其妙的问题，这里就修正掉并告警。
    // pool_size 尤其危险 —— 0 会让连接池永远借不到连接（死等）。
    if (this->port_ <= 0 || this->port_ > 65535) {
      LOG_WARN("mysql_config: invalid port {}, fallback to 3306", this->port_);
      this->SetPort(3306);
    }
    if (this->pool_size_ < 1) {
      LOG_WARN("mysql_config: invalid pool_size {}, fallback to 1",
               this->pool_size_);
      this->SetPoolSize(1);
    }
    if (this->connect_timeout_sec_ < 1) {
      LOG_WARN("mysql_config: invalid connect_timeout_sec {}, fallback to 3",
               this->connect_timeout_sec_);
      this->SetConnectTimeoutSec(3);
    }
    if (this->database_.empty()) {
      LOG_ERROR("mysql_config: database is empty");
      return false;
    }

    LOG_INFO("mysql_config loaded: {}", this->Describe());
    return true;
  } catch (const std::exception& e) {
    LOG_ERROR("mysql_config: load {} failed: {}", config_path, e.what());
    return false;
  }
}

std::string rpc::MysqlConfig::Describe() const {
  // 密码必须打码。日志泄露凭据是很常见的事故 ——
  // 日志往往会被采集、归档、转发，一旦打进去就等于公开了。
  const std::string masked_password =
      this->password_.empty() ? "(empty)" : "***";

  return "host=" + this->host_ + " port=" + std::to_string(this->port_) +
         " user=" + this->user_ + " password=" + masked_password +
         " db=" + this->database_ +
         " pool=" + std::to_string(this->pool_size_) +
         " connect_timeout=" + std::to_string(this->connect_timeout_sec_) + "s";
}

}  // namespace rpc
