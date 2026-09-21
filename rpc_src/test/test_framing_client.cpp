// 分帧改动的端到端验证（需要 ZK + server 已启动）。
//
//   ./build/framing_test
//
// 覆盖三件事：
//   1) 连续多次调用 —— 验证读缓冲被正确【消费】。
//      改之前客户端 recv_buf_ 永不清空（Connect::Read 只追加，唯一的
//      clear() 在服务端路径的 ProgressGetMessage 里），第二次 Call 就会
//      读到上一轮的残留字节而失败。
//   2) 大请求 —— 验证半包。请求体用随机十六进制，避免被 zstd 压得太小，
//      保证密文超过 MSS 从而触发 TCP 分段。
//   3) 请求/响应内容完整性 —— 大请求的 echo 必须原样返回。

#include <filesystem>
#include <iostream>
#include <random>
#include <string>

#include <nlohmann/json.hpp>

#include "rpc_client.h"

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

// 随机十六进制：四位一字符，压缩率低，适合构造"压不小"的载荷
std::string RandomHex(size_t chars) {
  static const char kHex[] = "0123456789abcdef";
  std::mt19937 gen(20260921);  // 固定种子，便于复现
  std::uniform_int_distribution<> dis(0, 15);
  std::string out;
  out.reserve(chars);
  for (size_t i = 0; i < chars; ++i) {
    out.push_back(kHex[dis(gen)]);
  }
  return out;
}

}  // namespace

int main() {
  std::cout << "══════ 分帧端到端测试 ══════" << std::endl;

  try {
    std::filesystem::path exe_dir =
        std::filesystem::canonical("/proc/self/exe").parent_path();
    std::filesystem::path config_dir = exe_dir / "../config";

    rpc::RpcClient client((config_dir / "client_config.json").string(),
                          (config_dir / "zk_config.json").string());

    // ── 用例 1：连续多次调用 ─────────────────────────────
    std::cout << "[1] 连续多次调用（验证读缓冲被消费）" << std::endl;
    for (int i = 0; i < 3; ++i) {
      const std::string msg = "call-" + std::to_string(i);
      nlohmann::json args = {{"message", msg}, {"user_id", std::to_string(i)}};
      nlohmann::json res;
      const bool ok = client.Call<nlohmann::json, nlohmann::json>(
          "RpcService", "echo", rpc::SerializerType::JSON, args, res);
      Check(ok && res.contains("receive message") &&
                res["receive message"].get<std::string>() == msg,
            "第 " + std::to_string(i + 1) + " 次调用返回正确（msg=" + msg + "）");
    }

    // ── 用例 2：大请求（触发半包）────────────────────────
    // 16KB 随机十六进制 → zstd 后仍有数 KB → 密文必然超过 MSS(1460)
    std::cout << "[2] 大请求（触发半包）" << std::endl;
    const size_t kBigChars = 16 * 1024;
    const std::string big = RandomHex(kBigChars);
    {
      nlohmann::json args = {{"message", big}, {"user_id", "big"}};
      nlohmann::json res;
      const bool ok = client.Call<nlohmann::json, nlohmann::json>(
          "RpcService", "echo", rpc::SerializerType::JSON, args, res);
      Check(ok, "大请求调用成功（" + std::to_string(kBigChars) + " 字符）");
      if (ok) {
        Check(res.contains("receive message") &&
                  res["receive message"].get<std::string>() == big,
              "大请求的 echo 内容原样返回（长度 "
                  + std::to_string(big.size()) + "）");
      }
    }

    // ── 用例 3：大请求之后还能正常小请求（缓冲无残留）────
    std::cout << "[3] 大请求之后再发小请求（验证缓冲无残留）" << std::endl;
    {
      nlohmann::json args = {{"message", "after-big"}, {"user_id", "x"}};
      nlohmann::json res;
      const bool ok = client.Call<nlohmann::json, nlohmann::json>(
          "RpcService", "echo", rpc::SerializerType::JSON, args, res);
      Check(ok && res.contains("receive message") &&
                res["receive message"].get<std::string>() == "after-big",
            "大请求之后的小请求仍然正确");
    }

  } catch (const std::exception& e) {
    std::cout << "❌ 测试异常退出: " << e.what() << std::endl;
    return 1;
  }

  std::cout << "════════════════════════════════" << std::endl;
  std::cout << "通过 " << g_pass << "，失败 " << g_fail << std::endl;
  return g_fail == 0 ? 0 : 1;
}
