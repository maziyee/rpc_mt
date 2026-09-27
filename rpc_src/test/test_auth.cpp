// AuthService 注册 / 登录测试。需要本机 MySQL 已启动，连接信息见
// config/mysql_config.json。
//
//   ./build/auth_test
//
// ⚠️ 所有断言只看 payload 里的 "error" 字段，【不】看 HandleRequest 的返回值。
//
//    因为计划中的统一调整（见 RPC_ERROR_HANDOFF.md 方案 A）会改变返回值的语义：
//    业务错误从 return false 改成 return true，错误一律留在 payload。若这里断言
//    返回值，那次调整会让测试全线变红，而它测的东西并没有坏。
//
//    这也正是验收标准 6 要求的调用方式：业务成败看 payload，返回值只表示
//    "这次调用送到了、被处理了"。
//
// 覆盖：注册成功 / 重名 / 弱密码 / 非法用户名 / 登录成功 / 密码错 /
//       用户不存在 / 库里存的是哈希而非明文 / 非法 JSON / 未知方法 / 清理

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "auth_service.h"
#include "log_manager.h"
#include "mysql_client.h"
#include "mysql_config.h"
#include "spdlog_config.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool ok, const std::string& what) {
  if (ok) {
    ++g_pass;
    std::cout << "  ✅ " << what << std::endl;
  } else {
    ++g_fail;
    std::cout << "  ❌ " << what << std::endl;
  }
}

// 测试数据统一用这个前缀，开头先按它清一遍，保证可重复跑。
// 不带下划线，这样 SQL 里能用 LIKE 'authtest%' 直接匹配，不用转义。
const char* const kPrefix = "authtest";

struct Reply {
  bool parsed = false;   // payload 是不是合法 JSON
  std::string error;     // "" 表示没有 error 字段，即业务成功
  nlohmann::json body;   // 完整 payload
};

// 调一次服务方法。刻意【丢弃】HandleRequest 的返回值 —— 理由见文件顶部的说明。
Reply Invoke(rpc::AuthService& svc, const std::string& method,
             const std::string& args) {
  std::string raw;
  (void)svc.HandleRequest(method, args, raw);
  Reply r;
  try {
    r.body = nlohmann::json::parse(raw);
    r.parsed = true;
    r.error = r.body.value("error", "");
  } catch (const std::exception&) {
    r.parsed = false;  // payload 不是 JSON —— 本身就是个失败信号
  }
  return r;
}

// 只想拿 error 字段时的简写
std::string Err(rpc::AuthService& svc, const std::string& method,
                const std::string& args) {
  return Invoke(svc, method, args).error;
}

std::string Creds(const std::string& user, const std::string& pass) {
  nlohmann::json j;
  j["username"] = user;
  j["password"] = pass;
  return j.dump();
}

}  // namespace

int main() {
  std::cout << "══════ AuthService 注册/登录测试 ══════" << std::endl;

  std::filesystem::path exe_dir =
      std::filesystem::canonical("/proc/self/exe").parent_path();
  std::filesystem::path config_dir = exe_dir / "../config";

  // auth_service.cpp / mysql_client.cpp 都用 LOG_*，这里按服务端的方式初始化
  rpc::SpdlogConfig spdlog_config;
  spdlog_config.InitSpdlog((config_dir / "spdlog_config.json").string());
  if (!rpc::Logger::GetInstance().Init(&spdlog_config)) {
    std::cerr << "❌ 初始化 Logger 失败" << std::endl;
    return 1;
  }

  rpc::MysqlConfig mysql_config;
  if (!mysql_config.Init((config_dir / "mysql_config.json").string())) {
    std::cerr << "❌ 读取 mysql_config.json 失败" << std::endl;
    return 1;
  }

  auto& db = rpc::MysqlClient::GetInstance();
  std::cout << "[1] MysqlClient Init" << std::endl;
  Check(db.Init(&mysql_config), "Init 成功");
  if (!db.IsAvailable()) {
    std::cout << "❌ 连不上 MySQL，后续用例没法进行。" << std::endl;
    return 1;
  }

  rpc::AuthService auth;

  // 先清一遍上次可能留下的数据，保证可重复跑
  {
    const int n = db.ExecuteParams("DELETE FROM users WHERE username LIKE ?",
                                   {std::string(kPrefix) + "%"});
    if (n > 0) {
      std::cout << "  ℹ️  清理了上次残留的 " << n << " 行" << std::endl;
    }
  }

  const std::string user_a = std::string(kPrefix) + "_alice";
  const std::string user_b = std::string(kPrefix) + "_bob";
  const std::string good_pw = "Pw12345678";

  // ── 2) 注册成功 ──
  std::cout << "[2] 注册成功" << std::endl;
  const Reply reg = Invoke(auth, "register", Creds(user_a, good_pw));
  Check(reg.parsed, "payload 是合法 JSON");
  Check(reg.error.empty(), "没有 error 字段（实际 '" + reg.error + "'）");
  Check(reg.body.contains("uid"), "返回了 uid");
  Check(reg.body.value("username", "") == user_a, "返回的 username 对得上");
  const std::string uid_a = reg.body.value("uid", "");
  Check(!uid_a.empty() && uid_a != "0", "uid 非空且非 0（实际 '" + uid_a + "'）");

  // ── 3) 重名 ──
  std::cout << "[3] 用户名重复" << std::endl;
  Check(Err(auth, "register", Creds(user_a, good_pw)) == "user_exists",
        "再次注册同一用户名 → user_exists");
  // 大小写不同也该撞上：users 表用的是 utf8mb4_0900_ai_ci
  Check(Err(auth, "register", Creds("AUTH" + user_a.substr(4), good_pw)) ==
            "user_exists",
        "大写变体也 → user_exists（MySQL 的 ci 排序规则）");

  // ── 4) 弱密码 ──
  std::cout << "[4] 密码不合规" << std::endl;
  Check(Err(auth, "register", Creds(user_b, "short")) == "weak_password",
        "7 位密码 → weak_password");
  Check(Err(auth, "register", Creds(user_b, "")) == "weak_password",
        "空密码 → weak_password");
  Check(Err(auth, "register", Creds(user_b, std::string(200, 'x'))) ==
            "weak_password",
        "200 位密码（超上界）→ weak_password");

  // ── 5) 用户名不合规 ──
  std::cout << "[5] 用户名不合规" << std::endl;
  Check(Err(auth, "register", Creds("ab", good_pw)) == "invalid_username",
        "2 位（太短）→ invalid_username");
  Check(Err(auth, "register", Creds(std::string(40, 'x'), good_pw)) ==
            "invalid_username",
        "40 位（太长）→ invalid_username");
  Check(Err(auth, "register", Creds("has space", good_pw)) ==
            "invalid_username",
        "含空格 → invalid_username");
  Check(Err(auth, "register", Creds("中文名字", good_pw)) == "invalid_username",
        "含非 ASCII → invalid_username");

  // ── 6) 登录成功 ──
  std::cout << "[6] 登录成功" << std::endl;
  const Reply login = Invoke(auth, "login", Creds(user_a, good_pw));
  Check(login.parsed, "payload 是合法 JSON");
  Check(login.error.empty(), "没有 error 字段（实际 '" + login.error + "'）");
  Check(login.body.value("uid", "") == uid_a,
        "登录返回的 uid 与注册时一致（" + uid_a + "）");

  // ── 7) 密码错 ──
  std::cout << "[7] 密码错" << std::endl;
  Check(Err(auth, "login", Creds(user_a, "WrongPassword1")) ==
            "invalid_credentials",
        "错密码 → invalid_credentials");

  // ── 8) 用户不存在 ──
  // 必须和 7 是【同一个】error —— 不同的值等于把接口做成用户名枚举器
  std::cout << "[8] 用户不存在" << std::endl;
  Check(Err(auth, "login", Creds("authtest_nobody", good_pw)) ==
            "invalid_credentials",
        "不存在的用户 → invalid_credentials（与密码错同一个值）");

  // ── 9) 库里存的是哈希，不是明文 ──
  // 这条是"我们真的没明文存密码"的直接证据，而不是靠读代码相信
  std::cout << "[9] 库里存的是哈希" << std::endl;
  std::vector<rpc::MysqlRow> rows;
  const int q = db.QueryParams(
      "SELECT password_hash, LENGTH(password_hash) FROM users WHERE username=?",
      {user_a}, rows);
  Check(q == 1 && !rows.empty(), "查到刚注册的那一行");
  if (!rows.empty() && rows[0].size() >= 2) {
    const std::string& stored = rows[0][0];
    Check(stored != good_pw, "存的不是明文密码");
    Check(stored.rfind("pbkdf2_sha256$", 0) == 0,
          "是 pbkdf2 编码串（" + stored.substr(0, 14) + "…）");
    Check(stored.size() <= 255, "长度没超 VARCHAR(255)（实际 " +
                                    std::to_string(stored.size()) + "）");
    // 用户表里不该有任何一列存着明文密码
    Check(stored.find(good_pw) == std::string::npos, "编码串里不含明文片段");
  }

  // ── 10) 非法输入 ──
  std::cout << "[10] 非法输入" << std::endl;
  Check(Err(auth, "register", "这不是 JSON") == "invalid_args",
        "args 不是 JSON → invalid_args");
  Check(Err(auth, "register", "") == "invalid_args", "空 args → invalid_args");
  Check(Err(auth, "register", R"({"username":"authtest_x"})") ==
            "invalid_args",
        "缺 password 字段 → invalid_args");
  Check(Err(auth, "register", R"({"username":123,"password":"Pw12345678"})") ==
            "invalid_args",
        "username 不是字符串 → invalid_args");
  Check(Err(auth, "login", "][") == "invalid_args",
        "login 的 args 不是 JSON → invalid_args");
  Check(Err(auth, "no_such_method", Creds(user_a, good_pw)) ==
            "unknown_method",
        "未知方法 → unknown_method（与业务错误可区分）");

  // ── 11) 清理 ──
  std::cout << "[11] 清理测试数据" << std::endl;
  const int del = db.ExecuteParams("DELETE FROM users WHERE username LIKE ?",
                                   {std::string(kPrefix) + "%"});
  Check(del >= 1, "删除测试用户（影响 " + std::to_string(del) + " 行）");

  std::cout << "════════════════════════════════" << std::endl;
  std::cout << "通过 " << g_pass << "，失败 " << g_fail << std::endl;
  return g_fail == 0 ? 0 : 1;
}
