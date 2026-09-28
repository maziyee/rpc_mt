#pragma once

#include <memory>
#include <string>

#include "service.h"
#include "token_store.h"

namespace rpc {

// 注册 / 登录 / 校验凭证。密码只以 PBKDF2 编码串落库，任何地方都不存明文。
//
// args / result 都是 JSON 字符串（与项目其他服务一致）：
//   register  {"username","password"} → {"uid","username"}
//   login     {"username","password"} → {"uid","username","token","expires_in"}
//   verify    {"token"}               → {"uid"}
//   logout    {"token"}               → {}
//
// result 里 "error" 的取值：
//   invalid_args          args 不是合法 JSON，或缺少字段
//   invalid_username      用户名不合规
//   weak_password         密码不合规
//   user_exists           用户名已被占用（register）
//   invalid_credentials   登录失败（login）。"用户不存在"和"密码错"【刻意】
//                         用同一个值 —— 用不同的值等于把接口做成用户名枚举器
//   invalid_token         token 无效、已过期或已被登出（verify）
//   unknown_method        method_name 不认识
//   server_error          数据库或 token 后端故障
//
// ⚠️ 实现必须在 .cpp 里。service.h 被网络层（connect.cpp、manager_cycle.cpp）
//    包含，往这个头文件塞 <mysql/mysql.h> 会让那些编译单元都去解析 MySQL 的头。
//    （token_store.h 是纯接口，只 <string>，所以可以放心包含。）
class AuthService : public Service {
 public:
  // 默认自建一个进程内的 token 存储。
  AuthService();

  // 注入外部存储，不接管所有权。用引用而不是指针：注入的东西不能为空，
  // 类型系统直接表达这件事，省掉一个运行期检查。
  // 测试用它塞假实现来覆盖 Verdict::kUnavailable 那条分支 ——
  // 进程内实现永远走不到那里，不注入就测不到。
  explicit AuthService(token::Store& store);

  ~AuthService() override;

  std::string GetServiceName() override { return "AuthService"; }
  bool HandleRequest(const std::string& method_name, const std::string& args,
                     std::string& result) override;

 private:
  bool Register(const std::string& args, std::string& result);
  bool Login(const std::string& args, std::string& result);
  bool Verify(const std::string& args, std::string& result);
  bool Logout(const std::string& args, std::string& result);

  std::unique_ptr<token::Store> owned_store_;  // 默认实现的所有权，可能为空
  token::Store* store_ = nullptr;              // 实际使用的（自有或外部注入）
};

}  // namespace rpc
