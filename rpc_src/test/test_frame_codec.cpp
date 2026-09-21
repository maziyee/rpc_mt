// frame_codec 的独立测试 —— 不依赖网络、不依赖 spdlog，纯逻辑。
//
// 编译运行（CMake 里还没有 test target，先手工编）：
//   g++ -std=c++17 -Iinclude rpc_src/test/test_frame_codec.cpp
//       rpc_src/common/network/frame_codec.cpp -o /tmp/test_frame_codec
//   /tmp/test_frame_codec

#include <arpa/inet.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "frame_codec.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool ok, const char* what) {
  if (ok) {
    ++g_pass;
    std::printf("  ✅ %s\n", what);
  } else {
    ++g_fail;
    std::printf("  ❌ %s\n", what);
  }
}

std::string MakePayload(size_t n, char fill) {
  return std::string(n, fill);
}

// ── 用例 1：编解码往返 ────────────────────────────────────
void TestRoundTrip() {
  std::printf("[1] 编解码往返\n");
  const std::string cipher = MakePayload(100, 'A');

  std::string buf = rpc::EncodeFrame(cipher);
  Check(buf.size() == rpc::FrameHeader::kSize + cipher.size(),
        "编码后长度 = 头(8) + 密文长度");

  std::string out;
  Check(rpc::TryDecodeFrame(buf, out) == rpc::FrameResult::kOk, "解码返回 kOk");
  Check(out == cipher, "解出的密文与原密文一致");
  Check(buf.empty(), "解码后 buf 被消费干净");
}

// ── 用例 2：半包（数据不够应安静等待，且不消费）──────────
void TestPartial() {
  std::printf("[2] 半包 —— 数据不够应返回 kNeedMore 且不消费\n");
  const std::string cipher = MakePayload(50, 'B');
  const std::string full = rpc::EncodeFrame(cipher);

  // 2a. 空缓冲
  {
    std::string buf;
    std::string out;
    Check(rpc::TryDecodeFrame(buf, out) == rpc::FrameResult::kNeedMore,
          "空缓冲 → kNeedMore");
  }

  // 2b. 只喂了头的一部分
  {
    std::string buf = full.substr(0, 4);
    const size_t before = buf.size();
    std::string out;
    Check(rpc::TryDecodeFrame(buf, out) == rpc::FrameResult::kNeedMore,
          "只有 4 字节头 → kNeedMore");
    Check(buf.size() == before, "半包时 buf 未被消费");
  }

  // 2c. 头齐了但密文还差 1 字节
  {
    std::string buf = full.substr(0, full.size() - 1);
    const size_t before = buf.size();
    std::string out;
    Check(rpc::TryDecodeFrame(buf, out) == rpc::FrameResult::kNeedMore,
          "密文差 1 字节 → kNeedMore");
    Check(buf.size() == before, "半包时 buf 未被消费");
  }

  // 2d. 最极端：逐字节喂入，最终应能解出
  {
    std::string buf;
    std::string out;
    bool got = false;
    for (char c : full) {
      buf.push_back(c);
      if (rpc::TryDecodeFrame(buf, out) == rpc::FrameResult::kOk) {
        got = true;
        break;
      }
    }
    Check(got && out == cipher, "逐字节喂入最终能解出（最极端的 TCP 分段）");
  }
}

// ── 用例 3：粘包（一次喂入多条，应逐条解出）──────────────
void TestCoalesced() {
  std::printf("[3] 粘包 —— 多条消息一次到达\n");
  const std::string c1 = MakePayload(30, 'C');
  const std::string c2 = MakePayload(200, 'D');
  const std::string c3 = MakePayload(5, 'E');

  std::string buf = rpc::EncodeFrame(c1) + rpc::EncodeFrame(c2) + rpc::EncodeFrame(c3);
  const size_t total = buf.size();

  std::string out;
  Check(rpc::TryDecodeFrame(buf, out) == rpc::FrameResult::kOk && out == c1,
        "第 1 条解出且内容正确");
  Check(rpc::TryDecodeFrame(buf, out) == rpc::FrameResult::kOk && out == c2,
        "第 2 条解出且内容正确");
  Check(rpc::TryDecodeFrame(buf, out) == rpc::FrameResult::kOk && out == c3,
        "第 3 条解出且内容正确");
  Check(buf.empty(), "三条解完后 buf 恰好为空（无残留、无丢字节）");
  Check(total == rpc::EncodeFrame(c1).size() + rpc::EncodeFrame(c2).size() +
                     rpc::EncodeFrame(c3).size(),
        "总字节数守恒");
}

// ── 用例 4：非法帧头 ─────────────────────────────────────
void TestInvalid() {
  std::printf("[4] 非法帧头应被拒绝\n");
  const std::string cipher = MakePayload(20, 'F');
  std::string out;

  // 4a. magic 不对
  {
    std::string buf = rpc::EncodeFrame(cipher);
    buf[0] = static_cast<char>(0xFF);
    Check(rpc::TryDecodeFrame(buf, out) == rpc::FrameResult::kInvalid,
          "magic 不匹配 → kInvalid");
  }

  // 4b. frame_len 为 0
  {
    std::string buf = rpc::EncodeFrame(cipher);
    buf[4] = 0;
    buf[5] = 0;
    buf[6] = 0;
    buf[7] = 0;
    Check(rpc::TryDecodeFrame(buf, out) == rpc::FrameResult::kInvalid,
          "frame_len = 0 → kInvalid");
  }

  // 4c. frame_len 超过上限（DoS 防护）
  {
    std::string buf = rpc::EncodeFrame(cipher);
    const uint32_t huge = rpc::FrameHeader::kMaxFrameSize + 1;
    const uint32_t net = htonl(huge);  // 和服务端读法一致：网络字节序
    std::memcpy(&buf[4], &net, sizeof(net));
    Check(rpc::TryDecodeFrame(buf, out) == rpc::FrameResult::kInvalid,
          "frame_len 超过上限 → kInvalid（防 DoS）");
  }

  // 4d. Peek 与 TryDecode 的判定一致
  {
    std::string buf = rpc::EncodeFrame(cipher);
    Check(rpc::PeekFrameState(buf) == rpc::FrameResult::kOk,
          "PeekFrameState 对完整帧返回 kOk");
    Check(rpc::PeekFrameState(buf.substr(0, 3)) == rpc::FrameResult::kNeedMore,
          "PeekFrameState 对不完整帧返回 kNeedMore");
  }
}

// ── 用例 5：零长度密文 ───────────────────────────────────
void TestEmptyCipher() {
  std::printf("[5] 零长度密文的边界行为\n");
  std::string buf = rpc::EncodeFrame("");
  std::string out;
  // 空密文编码后 frame_len = 0，应被判为非法（正常业务不会出现空密文）
  Check(rpc::TryDecodeFrame(buf, out) == rpc::FrameResult::kInvalid,
        "空密文 → kInvalid（frame_len = 0）");
}

}  // namespace

int main() {
  std::printf("══════ frame_codec 测试 ══════\n");
  TestRoundTrip();
  TestPartial();
  TestCoalesced();
  TestInvalid();
  TestEmptyCipher();
  std::printf("════════════════════════════════\n");
  std::printf("通过 %d，失败 %d\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
