// PBKDF2 密码哈希测试。纯计算，不需要 MySQL / Redis / 配置文件 / 日志初始化。
//
//   ./build/password_test          （接进 CMake 之后）
//   手工编译：见 test_frame_codec.cpp 顶部的同款做法 —— 把 test_password.cpp
//   和 security/password_hash.cpp 一起编，链 -lcrypto 即可，不需要 vcpkg。
//
// 覆盖：
//   1) 盐是随机的：同一密码两次 Hash 结果不同
//   2) 两次结果都能通过校验（盐随行存，不依赖任何全局状态）
//   3) 迭代数随行存：用非默认迭代数算的串，Verify 也必须认
//   4) 错密码被拒
//   5) 篡改编码串任意一段都被拒（算法名 / 迭代数 / 盐 / 派生密钥）
//   6) 格式非法的串被拒，且不能崩、不能卡住
//   7) 密码里的边界：空串 / 内嵌 \0 / 超长 / 非 ASCII
//   8) 迭代数实测耗时 —— 那是产品参数，不测出来没法判断要不要调
//
// 零依赖是刻意的：password_hash 只用 OpenSSL、不打日志，所以这个测试
// 不必拖进 spdlog / protobuf / zookeeper，编译两秒就能跑。

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include "password_hash.h"

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

// 把一个字符改成另一个【合法】的 hex 字符，用来做"看起来仍然合法但内容变了"
// 的篡改。直接用 'z' 之类非法字符的话，测的是格式校验而不是内容校验。
std::string TamperHex(const std::string& s, size_t pos) {
  std::string t = s;
  t[pos] = (t[pos] == 'a') ? 'b' : 'a';
  return t;
}

// 编码串的四段：算法 $ 迭代数 $ 盐 $ 派生密钥
struct Segments {
  std::string algo;
  std::string iters;
  std::string salt;
  std::string key;
};

bool Split(const std::string& encoded, Segments& out) {
  const size_t p1 = encoded.find('$');
  if (p1 == std::string::npos) return false;
  const size_t p2 = encoded.find('$', p1 + 1);
  if (p2 == std::string::npos) return false;
  const size_t p3 = encoded.find('$', p2 + 1);
  if (p3 == std::string::npos) return false;
  out.algo = encoded.substr(0, p1);
  out.iters = encoded.substr(p1 + 1, p2 - p1 - 1);
  out.salt = encoded.substr(p2 + 1, p3 - p2 - 1);
  out.key = encoded.substr(p3 + 1);
  return true;
}

}  // namespace

int main() {
  std::cout << "══════ PBKDF2 密码哈希测试 ══════" << std::endl;

  const std::string password = "correct horse battery staple";

  // ── 1) 盐是随机的 ──
  std::cout << "[1] 盐是随机的" << std::endl;
  const std::string h1 = rpc::password::Hash(password);
  const std::string h2 = rpc::password::Hash(password);
  Check(!h1.empty(), "Hash 返回非空");
  Check(!h2.empty(), "第二次 Hash 也非空");
  Check(h1 != h2, "同一密码两次结果不同（盐每次重新生成）");

  Segments s1{};
  Check(Split(h1, s1), "编码串能拆成四段");
  Check(s1.algo == "pbkdf2_sha256", "算法段是 pbkdf2_sha256");
  Check(s1.iters == std::to_string(rpc::password::kDefaultIterations),
        "迭代数段是默认值 " +
            std::to_string(rpc::password::kDefaultIterations));
  Check(s1.salt.size() == 32, "盐是 32 个 hex 字符（16 字节）");
  Check(s1.key.size() == 64, "派生密钥是 64 个 hex 字符（32 字节）");
  Segments s2{};
  Check(Split(h2, s2), "第二个串也能拆成四段");
  Check(s1.salt != s2.salt, "两次的盐不同");
  Check(s1.key != s2.key, "两次的派生密钥不同");

  // ── 2) 两次结果都能通过校验 ──
  // 这条验证的是"盐随行存"：如果 Verify 依赖某个全局盐，第二次就会失败。
  std::cout << "[2] 盐随行存，两次结果都能校验通过" << std::endl;
  const std::string h3 = rpc::password::Hash(password);
  Check(rpc::password::Verify(password, h1), "第一个哈希校验通过");
  Check(rpc::password::Verify(password, h2), "第二个哈希校验通过");
  Check(rpc::password::Verify(password, h3), "第三个哈希校验通过");

  // ── 3) 迭代数随行存 ──
  // 关键：Verify 必须用【串里记的】迭代数，不能写死默认值。
  // 否则将来调高默认值，所有存量用户立刻登不上。
  std::cout << "[3] 迭代数随行存（Verify 用的是串里的值）" << std::endl;
  const std::string h_low = rpc::password::Hash(password, 1000);
  Check(!h_low.empty(), "用 1000 次迭代生成成功");
  Check(rpc::password::Verify(password, h_low),
        "1000 次迭代的串能校验通过（若 Verify 写死默认值，这条会挂）");
  const std::string h_high = rpc::password::Hash(password, 50000);
  Check(rpc::password::Verify(password, h_high), "50000 次迭代的串能校验通过");
  Check(h_low != h_high, "不同迭代数得到不同的串");

  // ── 4) 错密码被拒 ──
  std::cout << "[4] 错密码被拒" << std::endl;
  Check(!rpc::password::Verify("wrong password", h1), "换一个密码 → 拒绝");
  Check(!rpc::password::Verify(password + " ", h1), "多一个空格 → 拒绝");
  Check(!rpc::password::Verify("Correct horse battery staple", h1),
        "大小写不同 → 拒绝（密码是大小写敏感的）");

  // ── 5) 篡改任意一段都被拒 ──
  std::cout << "[5] 篡改编码串任意一段都被拒" << std::endl;
  Check(!rpc::password::Verify(password, TamperHex(h1, 0)),
        "改算法名首字符 → 拒绝");
  Check(!rpc::password::Verify(
            password, s1.algo + "$100001$" + s1.salt + "$" + s1.key),
        "改迭代数（100000 → 100001）→ 拒绝");
  Check(!rpc::password::Verify(
            password, s1.algo + "$" + s1.iters + "$" + TamperHex(s1.salt, 5) +
                          "$" + s1.key),
        "改盐中间一个字符 → 拒绝");
  Check(!rpc::password::Verify(
            password, s1.algo + "$" + s1.iters + "$" + s1.salt + "$" +
                          TamperHex(s1.key, 63)),
        "改派生密钥末尾一个字符 → 拒绝");

  // ── 6) 格式非法的串：拒绝，且不崩不卡 ──
  // 用非默认迭代数的串做底，避免每条非法串都去算 10 万次迭代。
  std::cout << "[6] 格式非法的串被拒且不崩" << std::endl;
  const std::string low_ok = h_low;  // 1000 次迭代，便宜
  const auto t_bad_start = std::chrono::steady_clock::now();
  Check(!rpc::password::Verify(password, ""), "空串 → 拒绝");
  Check(!rpc::password::Verify(password, "随便一串中文"), "完全不是编码串 → 拒绝");
  Check(!rpc::password::Verify(password, "pbkdf2_sha256"), "只有一段 → 拒绝");
  Check(!rpc::password::Verify(password, "pbkdf2_sha256$1000"),
        "只有两段 → 拒绝");
  Check(!rpc::password::Verify(password, "pbkdf2_sha256$1000$" + s1.salt),
        "只有三段 → 拒绝");
  Check(!rpc::password::Verify(password, low_ok + "$多余的一段"),
        "五段（多一段）→ 拒绝");
  Check(!rpc::password::Verify(password, "scrypt$1000$" + s1.salt + "$" + s1.key),
        "算法名不认识 → 拒绝");
  Check(!rpc::password::Verify(
            password, "pbkdf2_sha256$1000$zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz$" +
                          s1.key),
        "盐里有非 hex 字符 → 拒绝");
  Check(!rpc::password::Verify(password, "pbkdf2_sha256$1000$abcd$" + s1.key),
        "盐长度不足 → 拒绝");
  Check(!rpc::password::Verify(
            password, "pbkdf2_sha256$1000$" + s1.salt + "$abcd"),
        "派生密钥长度不足 → 拒绝");
  Check(!rpc::password::Verify(password, "pbkdf2_sha256$0$" + s1.salt + "$" + s1.key),
        "迭代数 0 → 拒绝");
  Check(!rpc::password::Verify(
            password, "pbkdf2_sha256$-1$" + s1.salt + "$" + s1.key),
        "迭代数为负 → 拒绝");
  Check(!rpc::password::Verify(
            password, "pbkdf2_sha256$abc$" + s1.salt + "$" + s1.key),
        "迭代数非数字 → 拒绝");
  // 上界：这条必须【立刻】返回，不能真去算 1000 万次
  Check(!rpc::password::Verify(
            password, "pbkdf2_sha256$999999999$" + s1.salt + "$" + s1.key),
        "迭代数超过上界 → 拒绝");
  Check(!rpc::password::Verify(password, std::string(10000, 'a')),
        "一万字符的垃圾串 → 拒绝");
  const auto t_bad_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t_bad_start)
                            .count();
  Check(t_bad_ms < 1000,
        "以上 15 条非法输入总耗时 " + std::to_string(t_bad_ms) +
            " ms（<1000ms，说明没在非法输入上白算迭代）");

  // ── 7) 密码本身的边界 ──
  std::cout << "[7] 密码的边界情况" << std::endl;
  const std::string h_empty = rpc::password::Hash("");
  Check(rpc::password::Verify("", h_empty), "空密码能哈希并校验");
  Check(!rpc::password::Verify("x", h_empty), "空密码的串不接受非空密码");
  const std::string pw_nul("a\0b", 3);  // 内嵌 \0
  const std::string h_nul = rpc::password::Hash(pw_nul);
  Check(rpc::password::Verify(pw_nul, h_nul),
        "内嵌 \\0 的密码能校验（靠显式长度，不靠 c_str 截断）");
  Check(!rpc::password::Verify(std::string("a", 1), h_nul),
        "截断到 \\0 之前 → 拒绝（证明 \\0 之后的部分真的参与了）");
  const std::string pw_long(4096, 'x');
  Check(rpc::password::Verify(pw_long, rpc::password::Hash(pw_long)),
        "4096 字节的密码能往返");
  const std::string pw_utf8 = "密码🔒pässword";
  Check(rpc::password::Verify(pw_utf8, rpc::password::Hash(pw_utf8)),
        "非 ASCII / emoji 密码能往返");

  // ── 8) 迭代数耗时 ──
  std::cout << "[8] 迭代数实测耗时（产品参数）" << std::endl;
  const auto t0 = std::chrono::steady_clock::now();
  const std::string h_timed = rpc::password::Hash(password);
  const auto t1 = std::chrono::steady_clock::now();
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
  const long long per_sec = (ms > 0) ? (8000 / ms) : 0;  // 8 个 worker
  std::cout << "  ℹ️  " << rpc::password::kDefaultIterations << " 次迭代 ≈ "
            << ms << " ms" << std::endl;
  std::cout << "  ℹ️  按 8 个 worker 反推，登录吞吐上限 ≈ " << per_sec
            << " 次/秒（注册 + 登录各占一次）" << std::endl;
  Check(!h_timed.empty(), "计时用的那次 Hash 成功");

  std::cout << "════════════════════════════════" << std::endl;
  std::cout << "通过 " << g_pass << "，失败 " << g_fail << std::endl;
  return g_fail == 0 ? 0 : 1;
}
