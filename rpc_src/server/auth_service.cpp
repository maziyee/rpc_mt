#include "auth_service.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "log_manager.h"
#include "memory_token_store.h"
#include "mysql_client.h"
#include "password_hash.h"
#include "token_store.h"

namespace rpc {
namespace {

constexpr size_t kUsernameMinLen = 3;
constexpr size_t kUsernameMaxLen = 32;
constexpr size_t kPasswordMinLen = 8;
constexpr size_t kPasswordMaxLen = 128;

// ER_DUP_ENTRY。注册的唯一性靠 UNIQUE KEY 撞这个码，不做"先 SELECT 再 INSERT"。
constexpr int kErrDupEntry = -1062;

std::string ErrorResult(const std::string& code) {
  nlohmann::json j;
  j["error"] = code;
  return j.dump();
}

// 用户名限制在 [A-Za-z0-9_]。不是因为 MySQL 存不下别的，而是用户名会出现在
// 日志、ZK 路径、Redis key 里，收窄字符集能省掉一圈转义问题。
//
// 大小写不在这里处理：users 表用的是 utf8mb4_0900_ai_ci，MySQL 自己就认为
// Alice 和 alice 是同一个，注册会撞唯一键、登录也不区分大小写。
bool IsValidUsername(const std::string& s) {
  if (s.size() < kUsernameMinLen || s.size() > kUsernameMaxLen) {
    return false;
  }
  for (const char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_';
    if (!ok) {
      return false;
    }
  }
  return true;
}

// 只限长度，不强制"大小写+数字+符号至少三类"。
// NIST SP 800-63B 明确不推荐字符类规则 —— 用户会写成 P@ssw0rd1 这种同样
// 可预测的形式，还多了记不住的负担。它推荐的是"长度优先 + 常见弱口令黑名单"，
// 黑名单本轮不做。
bool IsValidPassword(const std::string& s) {
  return s.size() >= kPasswordMinLen && s.size() <= kPasswordMaxLen;
}

// 把 args 解析成一个 JSON 对象。失败时填好 result 并返回 false。
bool ParseObject(const std::string& args, nlohmann::json& out,
                 std::string& result) {
  try {
    out = nlohmann::json::parse(args);
  } catch (const std::exception& e) {
    LOG_WARN("AuthService: args is not valid json: {}", e.what());
    result = ErrorResult("invalid_args");
    return false;
  }
  if (!out.is_object()) {
    result = ErrorResult("invalid_args");
    return false;
  }
  return true;
}

// 从一个 JSON 对象里取字符串字段。缺字段或类型不对都算 invalid_args。
bool GetString(const nlohmann::json& in, const char* key, std::string& out,
               std::string& result) {
  if (!in.contains(key) || !in[key].is_string()) {
    result = ErrorResult("invalid_args");
    return false;
  }
  out = in[key].get<std::string>();
  return true;
}

// 解析出 username / password。
bool ParseCredentials(const std::string& args, std::string& username,
                      std::string& password, std::string& result) {
  nlohmann::json in;
  if (!ParseObject(args, in, result)) {
    return false;
  }
  return GetString(in, "username", username, result) &&
         GetString(in, "password", password, result);
}

// 解析出 token。
bool ParseToken(const std::string& args, std::string& token,
                std::string& result) {
  nlohmann::json in;
  if (!ParseObject(args, in, result)) {
    return false;
  }
  return GetString(in, "token", token, result);
}

}  // namespace

// ⚠️ 已知的两个用户名枚举通道，本轮【不】防护。
//
// ① register 返回 user_exists —— 直接通道，不用测时间。攻击者拿候选用户名去
//    注册，回"已存在"的就是真用户名。它比时序攻击好用得多，而且几乎关不掉：
//    不告诉用户"名字被占了"，注册体验会没法用。
//
// ② login 的两条路径耗时不同 —— 用户不存在时立刻返回，密码错时要跑约 25ms 的
//    PBKDF2。返回的 error 值已经统一成 invalid_credentials（消息层不泄露），
//    但耗时差 25 倍可以被统计出来。
//
// 只给 ② 打补丁（查不到用户时也跑一次 dummy Verify）收益有限，因为 ① 更容易
// 用。等做限流时一起处理 —— 限流本来就该同时覆盖 register 和 login。

AuthService::AuthService()
    : owned_store_(std::make_unique<token::MemoryStore>()),
      store_(owned_store_.get()) {}

AuthService::AuthService(token::Store& store) : store_(&store) {}

AuthService::~AuthService() = default;

bool AuthService::HandleRequest(const std::string& method_name,
                                const std::string& args, std::string& result) {
  if (method_name == "register") {
    return this->Register(args, result);
  }
  if (method_name == "login") {
    return this->Login(args, result);
  }
  if (method_name == "verify") {
    return this->Verify(args, result);
  }
  if (method_name == "logout") {
    return this->Logout(args, result);
  }
  LOG_ERROR("AuthService: unknown method '{}'", method_name);
  result = ErrorResult("unknown_method");
  return false;
}

bool AuthService::Register(const std::string& args, std::string& result) {
  std::string username;
  std::string password;
  if (!ParseCredentials(args, username, password, result)) {
    return false;
  }

  // 校验必须在哈希【之前】。一次 Hash 约 25ms，拿一个不合规的密码先算哈希
  // 再拒绝，就是白烧 CPU —— 8 个 worker 全被这种请求占住时服务就没用了。
  if (!IsValidUsername(username)) {
    result = ErrorResult("invalid_username");
    return false;
  }
  if (!IsValidPassword(password)) {
    result = ErrorResult("weak_password");
    return false;
  }

  const std::string encoded = password::Hash(password);
  if (encoded.empty()) {
    LOG_ERROR("AuthService::register: hash failed");
    result = ErrorResult("server_error");
    return false;
  }

  auto& db = MysqlClient::GetInstance();
  if (!db.IsAvailable()) {
    LOG_ERROR("AuthService::register: mysql unavailable");
    result = ErrorResult("server_error");
    return false;
  }

  // 唯一性靠 UNIQUE KEY 撞错误码。"先 SELECT 再 INSERT"是 TOCTOU：两个并发
  // 注册都能通过 SELECT 检查，然后都去 INSERT。
  uint64_t uid = 0;
  const int n = db.InsertParams(
      "INSERT INTO users (username, password_hash) VALUES (?, ?)",
      {username, encoded}, uid);

  if (n == kErrDupEntry) {
    result = ErrorResult("user_exists");  // 正常业务分支，不是故障
    return false;
  }
  if (n < 0) {
    LOG_ERROR("AuthService::register: insert failed, code={}", n);
    result = ErrorResult("server_error");
    return false;
  }

  nlohmann::json out;
  out["uid"] = std::to_string(uid);
  out["username"] = username;
  result = out.dump();
  return true;
}

bool AuthService::Login(const std::string& args, std::string& result) {
  std::string username;
  std::string password;
  if (!ParseCredentials(args, username, password, result)) {
    return false;
  }

  // 这里【不】做用户名/密码格式校验。登录时格式不对就是"凭证不对"，
  // 专门告诉对方"你这个用户名格式非法"是多送了一条信息。

  auto& db = MysqlClient::GetInstance();
  if (!db.IsAvailable()) {
    LOG_ERROR("AuthService::login: mysql unavailable");
    result = ErrorResult("server_error");
    return false;
  }

  std::vector<MysqlRow> rows;
  const int n = db.QueryParams(
      "SELECT uid, password_hash FROM users WHERE username = ?", {username},
      rows);
  if (n < 0) {
    LOG_ERROR("AuthService::login: query failed, code={}", n);
    result = ErrorResult("server_error");
    return false;
  }

  // 查不到用户和密码错返回【同一个】error —— 用不同的值就是把接口做成
  // 用户名枚举器。但耗时仍然不同，见文件顶部 ② 的说明。
  if (rows.empty() || rows[0].size() < 2) {
    result = ErrorResult("invalid_credentials");
    return false;
  }

  const std::string& uid = rows[0][0];
  const std::string& encoded = rows[0][1];
  if (!password::Verify(password, encoded)) {
    // 只记用户名，绝不记密码
    LOG_WARN("AuthService::login: wrong password for '{}'", username);
    result = ErrorResult("invalid_credentials");
    return false;
  }

  // 密码对了 —— 签发 token。
  const std::string token_str = this->store_->Issue(uid, token::kDefaultTtlSec);
  if (token_str.empty()) {
    // ⚠️ 签发失败必须当【登录失败】处理。返回一个空 token 说"登录成功"，
    //    用户下一步必然 401，而且从响应里看不出原因。
    LOG_ERROR("AuthService::login: token issue failed, uid={}", uid);
    result = ErrorResult("server_error");
    return false;
  }

  nlohmann::json out;
  out["uid"] = uid;
  out["username"] = username;
  out["token"] = token_str;
  out["expires_in"] = token::kDefaultTtlSec;
  result = out.dump();
  return true;
}

bool AuthService::Verify(const std::string& args, std::string& result) {
  std::string token_str;
  if (!ParseToken(args, token_str, result)) {
    return false;
  }

  std::string uid;
  const token::Verdict v = this->store_->Verify(token_str, uid);

  if (v == token::Verdict::kUnavailable) {
    // 后端挂了。这条【必须】和"token 无效"分开：回 401 会让用户以为"重新
    // 登录就行"，但重登也登不上（后端本身不可用），而且运维那边没有告警。
    // 进程内实现走不到这个分支，换 Redis 之后才会。
    LOG_ERROR("AuthService::verify: token backend unavailable");
    result = ErrorResult("server_error");
    return false;
  }
  if (v != token::Verdict::kValid) {
    result = ErrorResult("invalid_token");
    return false;
  }

  // 只回 uid，不查库 —— verify 在每个请求上都调，不能带一次 MySQL 往返。
  // 需要 username 的地方（显示"我是谁"之类）单独查，那是低频操作。
  nlohmann::json out;
  out["uid"] = uid;
  result = out.dump();
  return true;
}

bool AuthService::Logout(const std::string& args, std::string& result) {
  std::string token_str;
  if (!ParseToken(args, token_str, result)) {
    return false;
  }
  // 幂等：撤一个不存在或已过期的 token 不算错误 —— 重复登出是正常操作。
  //
  // ⚠️ Revoke 返回 void，所以后端不可用时这里【看不出来】：客户端以为登出
  //    成功了，而 token 其实还有效（直到 TTL 到期）。进程内实现没有这个问题；
  //    换 Redis 后如果要严格，得让 Revoke 也返回三态。
  this->store_->Revoke(token_str);
  result = nlohmann::json::object().dump();
  return true;
}

}  // namespace rpc
