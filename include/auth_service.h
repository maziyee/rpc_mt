#pragma once

#include <string>

#include "service.h"

namespace rpc {

// 注册 / 登录。密码只以 PBKDF2 编码串落库，任何地方都不存明文。
//
// args / result 都是 JSON 字符串（与项目其他服务一致）：
//   register  {"username","password"}  → {"uid","username"} 或 {"error"}
//   login     {"username","password"}  → {"uid","username"} 或 {"error"}
//
// result 里 "error" 的取值：
//   invalid_args          args 不是合法 JSON，或缺少字段
//   invalid_username      用户名不合规
//   weak_password         密码不合规
//   user_exists           用户名已被占用（register）
//   invalid_credentials   登录失败（login）。"用户不存在"和"密码错"【刻意】
//                         用同一个值 —— 用不同的值等于把接口做成用户名枚举器
//   unknown_method        method_name 不认识
//   server_error          数据库故障
//
// ⚠️ 实现必须在 .cpp 里。service.h 被网络层（connect.cpp、manager_cycle.cpp）
//    包含，往这个头文件塞 <mysql/mysql.h> 会让那些编译单元都去解析 MySQL 的头。
class AuthService : public Service {
 public:
  AuthService() = default;
  ~AuthService() override = default;

  std::string GetServiceName() override { return "AuthService"; }
  bool HandleRequest(const std::string& method_name, const std::string& args,
                     std::string& result) override;

 private:
  bool Register(const std::string& args, std::string& result);
  bool Login(const std::string& args, std::string& result);
};

}  // namespace rpc
