// MySQL 连接池测试（需要本机 MySQL 已启动，连接信息见 config/mysql_config.json）。
//
//   ./build/mysql_test
//
// 覆盖：
//   1) Init：建库建表 + 建连接池
//   2) InsertParams 带出 AUTO_INCREMENT 主键，且与回查结果一致
//   3) 唯一键冲突返回 -1062（ER_DUP_ENTRY）
//   4) CLIENT_FOUND_ROWS：把字段更新成原值，返回 1 而不是 0
//   5) 并发借用/归还：线程数是池大小的两倍，有连接漏归还就会 acquire 超时
//   6) Close：关闭池
//   7) 重新 Init 后清理测试数据
//
// 用例 2~5 都在验证「借用即 RAII」这条改动：连接借出后无论成功还是失败、
// 无论从哪条路径返回，都必须回到池子里。

#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

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

// 测试数据统一用这个前缀，开头先按它清一遍，保证测试可重复跑
const char* const kUserPrefix = "mysql_test_user";

}  // namespace

int main() {
  std::cout << "══════ MySQL 连接池测试 ══════" << std::endl;

  try {
    std::filesystem::path exe_dir =
        std::filesystem::canonical("/proc/self/exe").parent_path();
    std::filesystem::path config_dir = exe_dir / "../config";

    // mysql_client.cpp 用 LOG_* 打日志，这里按服务端的方式初始化，
    // 否则看不到 "mysql_client: ready, pool=N" 这类关键信息
    rpc::SpdlogConfig spdlog_config;
    spdlog_config.InitSpdlog((config_dir / "spdlog_config.json").string());
    if (!rpc::Logger::GetInstance().Init(&spdlog_config)) {
      std::cerr << "初始化日志失败" << std::endl;
    }

    rpc::MysqlConfig mysql_config;
    if (!mysql_config.Init((config_dir / "mysql_config.json").string())) {
      std::cout << "❌ 读取 mysql_config.json 失败" << std::endl;
      return 1;
    }

    rpc::MysqlClient& client = rpc::MysqlClient::GetInstance();

    // ── 用例 1：Init ─────────────────────────────────────
    std::cout << "[1] Init：建库建表 + 建连接池" << std::endl;
    const bool inited = client.Init(&mysql_config);
    Check(inited, "Init 成功");
    Check(client.IsAvailable(), "IsAvailable() 为真");
    if (!inited) {
      std::cout << "❌ 连不上 MySQL，后续用例没法进行。请确认 "
                << mysql_config.GetHost() << ":" << mysql_config.GetPort()
                << " 上的 MySQL 已启动（用户 " << mysql_config.GetUser()
                << "，库 " << mysql_config.GetDatabase() << "）" << std::endl;
      return 1;
    }

    // 名字固定，开头按前缀清一次，重复跑不会冲突
    const std::string username = std::string(kUserPrefix) + "_1";
    const std::string like_prefix = std::string(kUserPrefix) + "%";
    client.ExecuteParams("DELETE FROM users WHERE username LIKE ?",
                         {like_prefix});

    // ── 用例 2：插入带出主键 ─────────────────────────────
    std::cout << "[2] InsertParams：带出 AUTO_INCREMENT 主键" << std::endl;
    uint64_t uid = 0;
    const int ins = client.InsertParams(
        "INSERT INTO users (username, password_hash) VALUES (?, ?)",
        {username, "hash-by-test"}, uid);
    Check(ins == 1, "插入返回 1 行（实际 " + std::to_string(ins) + "）");
    Check(uid > 0, "带出 insert_id > 0（实际 " + std::to_string(uid) + "）");

    // 回查：带出的 insert_id 必须就是这一行的真实主键。这正是它不能靠
    // SELECT LAST_INSERT_ID() 事后补的原因 —— 那是会话级的，池里下一次
    // 借到的很可能是另一条连接。
    std::vector<rpc::MysqlRow> rows;
    const int q = client.QueryParams("SELECT uid FROM users WHERE username = ?",
                                     {username}, rows);
    Check(q == 1, "按 username 回查命中 1 行（实际 " + std::to_string(q) + "）");
    if (q == 1 && !rows.empty() && !rows[0].empty()) {
      Check(rows[0][0] == std::to_string(uid),
            "回查的 uid 与 insert_id 一致（" + rows[0][0] + " vs " +
                std::to_string(uid) + "）");
    }

    // ── 用例 3：唯一键冲突 ───────────────────────────────
    std::cout << "[3] 唯一键冲突：期望 -1062（ER_DUP_ENTRY）" << std::endl;
    uint64_t dup_id = 0;
    const int dup = client.InsertParams(
        "INSERT INTO users (username, password_hash) VALUES (?, ?)",
        {username, "another-hash"}, dup_id);
    Check(dup == -1062, "返回 -1062（实际 " + std::to_string(dup) + "）");
    Check(dup_id == 0, "失败时不改出参（dup_id 仍为 0）");

    // ── 用例 4：CLIENT_FOUND_ROWS ────────────────────────
    std::cout << "[4] CLIENT_FOUND_ROWS：更新成原值应返回 1" << std::endl;
    const int upd = client.ExecuteParams(
        "UPDATE users SET password_hash = ? WHERE username = ?",
        {"hash-by-test", username});
    Check(upd == 1,
          "更新成原值返回 1（实际 " + std::to_string(upd) +
              "；若为 0 说明建连没带 CLIENT_FOUND_ROWS）");

    // ── 用例 5：并发借用/归还 ────────────────────────────
    // 线程数取池大小的两倍。只要有一条连接被漏归还，后面的线程就会一直等到
    // 3 秒 acquire 超时并返回 kNoConnection —— 这是「借用即 RAII」唯一能真
    // 跑出来的证据，也是这次改动最该守住的行为。
    const int threads_count = mysql_config.GetPoolSize() * 2;
    std::cout << "[5] 并发借用/归还：" << threads_count << " 线程抢 "
              << mysql_config.GetPoolSize() << " 条连接" << std::endl;
    std::vector<int> codes(static_cast<size_t>(threads_count),
                           rpc::MysqlClient::kNoConnection);
    {
      std::vector<std::thread> threads;
      threads.reserve(static_cast<size_t>(threads_count));
      for (int i = 0; i < threads_count; ++i) {
        threads.emplace_back([&client, &codes, &username, i]() {
          std::vector<rpc::MysqlRow> r;
          codes[i] = client.QueryParams(
              "SELECT uid FROM users WHERE username = ?", {username}, r);
        });
      }
      for (std::thread& t : threads) {
        t.join();
      }
    }
    int ok_count = 0;
    for (int code : codes) {
      if (code == 1) {
        ++ok_count;
      }
    }
    Check(ok_count == threads_count,
          std::to_string(threads_count) + " 个线程全部查询成功（成功 " +
              std::to_string(ok_count) + " 个）");

    // ── 用例 6：Close ────────────────────────────────────
    std::cout << "[6] Close：关闭池" << std::endl;
    client.Close();
    Check(!client.IsAvailable(), "Close 后 IsAvailable() 为假");

    // ── 用例 7：清理 ─────────────────────────────────────
    // 池已作废，先重新 Init（顺带验证 Close 之后还能重建），再删测试数据
    std::cout << "[7] 重新 Init 后清理测试数据" << std::endl;
    if (client.Init(&mysql_config)) {
      const int del = client.ExecuteParams(
          "DELETE FROM users WHERE username LIKE ?", {like_prefix});
      Check(del >= 1, "删除测试用户（影响 " + std::to_string(del) + " 行）");
    } else {
      Check(false, "Close 之后重新 Init 失败");
    }

  } catch (const std::exception& e) {
    std::cout << "❌ 测试异常退出: " << e.what() << std::endl;
    return 1;
  }

  std::cout << "════════════════════════════════" << std::endl;
  std::cout << "通过 " << g_pass << "，失败 " << g_fail << std::endl;
  return g_fail == 0 ? 0 : 1;
}
