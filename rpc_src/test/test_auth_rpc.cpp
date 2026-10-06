// AuthService 走【真 RPC】的端到端测试。
//
// 需要三样都起着：
//   sudo service mysql start
//   ./scripts/zk_server.sh daemon
//   ./build/server
// 然后：./build/auth_rpc_test
//
// ── 为什么要有这个文件 ──
// test_auth.cpp 的 61 条全是【直接 new 服务对象】调的，从没走过网络。它验证
// 不了：RpcResponse 的 payload 能不能到客户端、服务在线程池里能不能跑、多个
// worker 并发打 MysqlClient 会不会踩。这些只有走真 RPC 才暴露。
//
// ── 本文件同时是错误链路的端到端验收 ──
// 每个业务错误都断言两件事：Call 返回 false，且出参里能读到 {"error":...}。
// 这两条对应修复前的两个缺陷（服务端丢 result / 客户端无条件 return true）。
// 断掉任何一个，这里会成片变红，而不是只挂一两条。

#include <filesystem>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "log_manager.h"
#include "mysql_client.h"
#include "mysql_config.h"
#include "rpc_client.h"
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

// 本测试用的用户名前缀。清理只删这个前缀的行，不碰别人的数据。
// 必须清理：不清的话第二次跑 [1] 就会撞 user_exists 而失败。
const char* const kPrefix = "rpcauthtest";

nlohmann::json Creds(const std::string& u, const std::string& p) {
  return nlohmann::json{{"username", u}, {"password", p}};
}

nlohmann::json TokenArg(const std::string& t) {
  return nlohmann::json{{"token", t}};
}

// 调 AuthService 并把 payload 解出来。
//
// ⚠️ Call 返回 false 时 payload 【仍然有效】—— 这是错误链路修复定下的契约
//    （服务端失败时也发 result_data_）。业务错误码就在这里读。
struct Reply {
  bool call_ok = false;
  nlohmann::json body;
  std::string error;  // body["error"]，没有就是空串
};

Reply Invoke(rpc::RpcClient& client, const std::string& method,
             const nlohmann::json& args) {
  Reply r;
  r.call_ok = client.Call<nlohmann::json, nlohmann::json>(
      "AuthService", method, rpc::SerializerType::JSON, args, r.body);
  if (r.body.is_object() && r.body.contains("error")) {
    r.error = r.body["error"].get<std::string>();
  }
  return r;
}

// 直连 MySQL 清掉本测试自己的行。测试进程和 server 是两个 MysqlClient 实例，
// 各有各的连接池，同删一张表互不影响。
int Cleanup(rpc::MysqlClient& db) {
  return db.ExecuteParams("DELETE FROM users WHERE username LIKE ?",
                          {std::string(kPrefix) + "%"});
}

std::string Upper(const std::string& s) {
  std::string out = s;
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return out;
}

}  // namespace

int main() {
  std::cout << "══════ AuthService 真 RPC 端到端测试 ══════" << std::endl;

  std::filesystem::path exe_dir =
      std::filesystem::canonical("/proc/self/exe").parent_path();
  std::filesystem::path config_dir = exe_dir / "../config";

  // auth_service.cpp / mysql_client.cpp 都用 LOG_*，按服务端的方式初始化
  rpc::SpdlogConfig spdlog_config;
  spdlog_config.InitSpdlog((config_dir / "spdlog_config.json").string());
  if (!rpc::Logger::GetInstance().Init(&spdlog_config)) {
    std::cerr << "❌ 初始化 Logger 失败" << std::endl;
    return 1;
  }

  // ── [0] 直连 MySQL 清掉上次残留 ──────────────────────
  rpc::MysqlConfig mysql_config;
  if (!mysql_config.Init((config_dir / "mysql_config.json").string())) {
    std::cerr << "❌ 读取 mysql_config.json 失败" << std::endl;
    return 1;
  }
  auto& db = rpc::MysqlClient::GetInstance();
  if (!db.Init(&mysql_config) || !db.IsAvailable()) {
    std::cerr << "❌ 连不上 MySQL —— 这个测试需要它起着"
              << "（sudo service mysql start）" << std::endl;
    return 1;
  }
  {
    const int n = Cleanup(db);
    if (n > 0) {
      std::cout << "  ℹ️  清理了上次残留的 " << n << " 行" << std::endl;
    }
  }

  try {
    rpc::RpcClient client((config_dir / "client_config.json").string(),
                          (config_dir / "zk_config.json").string());
    if (!client.IsConnect()) {
      std::cerr << "❌ 连不上 server —— 先起 ./build/server" << std::endl;
      return 1;
    }

    const std::string user = std::string(kPrefix) + "_alice";
    const std::string good_pw = "Pw12345678";
    std::string uid;
    std::string token;

    // ── [1] 注册成功 ─────────────────────────────────
    std::cout << "[1] register 新用户" << std::endl;
    {
      const Reply r = Invoke(client, "register", Creds(user, good_pw));
      Check(r.call_ok, "Call 返回 true");
      Check(r.error.empty(), "没有 error 字段（实际 '" + r.error + "'）");
      Check(r.body.contains("uid"), "返回了 uid");
      Check(r.body.contains("username") && r.body["username"] == user,
            "返回了 username");
      if (r.body.contains("uid")) {
        uid = r.body["uid"].get<std::string>();
      }
    }

    // ── [2] 同名再注册 → user_exists ──────────────────
    std::cout << "[2] 同名再 register" << std::endl;
    {
      const Reply r = Invoke(client, "register", Creds(user, good_pw));
      Check(!r.call_ok, "Call 返回 false");
      Check(r.error == "user_exists",
            "error == user_exists（实际 '" + r.error + "'）");
    }

    // ── [3] 大小写不敏感（utf8mb4_0900_ai_ci）─────────
    std::cout << "[3] 用户名大小写不敏感" << std::endl;
    {
      const Reply r = Invoke(client, "register", Creds(Upper(user), good_pw));
      Check(!r.call_ok && r.error == "user_exists",
            "大写形式也撞 user_exists —— 防大小写冒充");
    }

    // ── [4] 非法用户名 ──────────────────────────────
    std::cout << "[4] 非法用户名（含空格）" << std::endl;
    {
      const Reply r =
          Invoke(client, "register", Creds(kPrefix + std::string(" bad"),
                                           good_pw));
      Check(!r.call_ok && r.error == "invalid_username",
            "error == invalid_username（实际 '" + r.error + "'）");
    }

    // ── [5] 弱密码 ──────────────────────────────────
    std::cout << "[5] 弱密码（短于 8 位）" << std::endl;
    {
      const Reply r =
          Invoke(client, "register", Creds(std::string(kPrefix) + "_short",
                                           "abc"));
      Check(!r.call_ok && r.error == "weak_password",
            "error == weak_password（实际 '" + r.error + "'）");
    }

    // ── [6] 登录成功 ────────────────────────────────
    std::cout << "[6] login 正确密码" << std::endl;
    {
      const Reply r = Invoke(client, "login", Creds(user, good_pw));
      Check(r.call_ok, "Call 返回 true");
      Check(r.error.empty(), "没有 error 字段");
      Check(r.body.contains("token"), "返回了 token");
      Check(r.body.contains("expires_in"), "返回了 expires_in");
      Check(r.body.contains("uid") && r.body["uid"] == uid,
            "uid 和注册时一致");
      if (r.body.contains("token")) {
        token = r.body["token"].get<std::string>();
      }
    }

    // ── [7] 密码错 ──────────────────────────────────
    std::cout << "[7] login 密码错" << std::endl;
    {
      const Reply r = Invoke(client, "login", Creds(user, "WrongPass123"));
      Check(!r.call_ok, "Call 返回 false");
      Check(r.error == "invalid_credentials",
            "error == invalid_credentials（实际 '" + r.error + "'）");
    }

    // ── [8] 用户不存在 → 和密码错【同一个】error ────────
    std::cout << "[8] login 用户不存在" << std::endl;
    {
      const Reply r = Invoke(client, "login",
                             Creds(std::string(kPrefix) + "_nobody", good_pw));
      Check(!r.call_ok && r.error == "invalid_credentials",
            "和密码错返回同一个 error —— 否则接口就是个用户名枚举器");
    }

    // ── [9] verify ──────────────────────────────────
    std::cout << "[9] verify 刚签发的 token" << std::endl;
    if (token.empty()) {
      Check(false, "拿不到 token，后面的用例没法进行");
    } else {
      const Reply r = Invoke(client, "verify", TokenArg(token));
      Check(r.call_ok, "Call 返回 true");
      Check(r.body.contains("uid") && r.body["uid"] == uid,
            "verify 返回的 uid 和登录的一致");
    }

    // ── [10] 未知方法 ───────────────────────────────
    std::cout << "[10] 未知方法" << std::endl;
    {
      const Reply r =
          Invoke(client, "no_such_method", nlohmann::json::object());
      Check(!r.call_ok && r.error == "unknown_method",
            "error == unknown_method（实际 '" + r.error + "'）");
    }

    // ── [11] args 不是 JSON 对象 ────────────────────
    std::cout << "[11] args 不是 JSON 对象" << std::endl;
    {
      const Reply r = Invoke(client, "login", "a string, not an object");
      Check(!r.call_ok && r.error == "invalid_args",
            "error == invalid_args（实际 '" + r.error + "'）");
    }

    // ── [12] 登出后 token 失效 ──────────────────────
    std::cout << "[12] logout 之后 verify" << std::endl;
    if (token.empty()) {
      Check(false, "拿不到 token，跳过");
    } else {
      const Reply out = Invoke(client, "logout", TokenArg(token));
      Check(out.call_ok, "logout 返回 true（幂等，撤不存在的也不算错）");

      const Reply r = Invoke(client, "verify", TokenArg(token));
      Check(!r.call_ok && r.error == "invalid_token",
            "同一个 token 现在 invalid_token（实际 '" + r.error + "'）");
    }

  } catch (const std::exception& e) {
    std::cout << "❌ 测试异常退出: " << e.what() << std::endl;
    return 1;
  }

  // ── [13] 清理 ─────────────────────────────────────
  {
    const int n = Cleanup(db);
    std::cout << "  ℹ️  清理了 " << n << " 行" << std::endl;
  }

  std::cout << "════════════════════════════════" << std::endl;
  std::cout << "通过 " << g_pass << "，失败 " << g_fail << std::endl;
  return g_fail == 0 ? 0 : 1;
}
