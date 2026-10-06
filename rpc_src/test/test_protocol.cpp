// 协议 type 字段 + Connect 按类型分派。
//
//   ./build/protocol_test
//
// 为什么需要这个文件：加 type_ 之前，一条帧是 request 还是 response
// 【无法分辨】—— 按错的类型解析不报错，只会解出乱码（A 组最后一条把这件事
// 演示出来了）。A 组证明类型字段被正确写入、正确校验；B 组证明 Connect 会
// 按它分派到不同的回调。
//
// B 组是目前唯一覆盖 connect.cpp 里 response 分派分支的东西：framing_test
// 覆盖不到它（客户端的响应走 rpc_client.h 的 ProcessResponse，不经过
// ProgressGetMessage）。

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include "aes_encrypt.h"
#include "connect.h"
#include "frame_codec.h"
#include "rpc_protobuf.h"
#include "zstd_compress.h"

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

// ── 造工具 ────────────────────────────────────────────────

// 造一对已连接的回环 TCP socket。mine 是"我们的"一端，peer 是对端。
//
// 用真 TCP 而不是 socketpair(AF_UNIX)：Connect 的构造里 getpeername +
// 按 AF_INET 解析地址，AF_UNIX 的地址结构对不上。
//
// mine 设成非阻塞 —— Connect::ReadTheInfo 靠 EAGAIN 判断"读完了"，
// 阻塞 socket 会直接卡在那里。
bool MakeTcpPair(int& mine, int& peer) {
  const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;  // 让内核挑一个空闲端口
  if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
      ::listen(listen_fd, 1) < 0) {
    ::close(listen_fd);
    return false;
  }
  socklen_t alen = sizeof(addr);
  if (::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &alen) < 0) {
    ::close(listen_fd);
    return false;
  }

  const int c = ::socket(AF_INET, SOCK_STREAM, 0);
  if (c < 0 ||
      ::connect(c, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(listen_fd);
    if (c >= 0) {
      ::close(c);
    }
    return false;
  }
  const int s = ::accept(listen_fd, nullptr, nullptr);
  ::close(listen_fd);
  if (s < 0) {
    ::close(c);
    return false;
  }

  const int flags = ::fcntl(c, F_GETFL, 0);
  ::fcntl(c, F_SETFL, flags | O_NONBLOCK);

  mine = c;
  peer = s;
  return true;
}

// 序列化 → 压缩 → 加密 → 分帧。把 Connect::Write 里那几步拆出来，直接返回
// 字节，好在测试里手工控制往对端写什么。
std::string EncodeWire(rpc::RpcBase& msg) {
  std::string serialized;
  msg.Serializer(serialized);
  std::string compressed;
  rpc::ZstdCompress::GetInstance().CompressString(serialized, compressed);
  std::string cipher;
  rpc::AesEncrypt::GetInstance().Encrypt(compressed, cipher);
  return rpc::EncodeFrame(cipher);
}

// 一次写干净。peer 是 accept() 出来的阻塞 socket，小数据不会 EAGAIN。
bool SendAll(int fd, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t n =
        ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

// 反复 Read() 直到 recv_buf_ 凑够 want 字节。
//
// 为什么不能调一次 Read() 就完事：回环 TCP 上 send() 返回**不代表**对端已经
// 能 recv() 到 —— 段还在协议栈里走。调一次就断言的话会读到 0 字节，然后
// 表现为"帧根本没解出来"，很容易误判成协议代码有问题。
bool PumpUntil(rpc::Connect& conn, size_t want, int max_ms) {
  for (int waited = 0; waited <= max_ms; ++waited) {
    conn.Read();
    if (conn.GetReadBuf().size() >= want) {
      return true;
    }
    ::usleep(1000);
  }
  return false;
}

rpc::RpcRequest MakeRequest(const std::string& service,
                            const std::string& method,
                            const std::string& payload,
                            uint32_t seq) {
  rpc::RpcRequest req;
  req.SetServiceName(service);
  req.SetMethodName(method);
  req.SetPayload(payload);
  req.SetSequenceId(seq);
  return req;
}

rpc::RpcResponse MakeResponse(const std::string& result, uint32_t error_code,
                              const std::string& error_message, uint32_t seq) {
  rpc::RpcResponse resp;
  resp.SetResultData(result);
  resp.SetErrorCode(error_code);
  resp.SetErrorMessage(error_message);
  resp.SetSequenceId(seq);
  return resp;
}

// ── A 组：协议层 ──────────────────────────────────────────

void TestLayout() {
  std::cout << "[A1] RpcHeader 布局" << std::endl;
  // 这四条是【线上格式的契约】。改坏了不会有编译错误，只会让新旧二进制
  // 互相看不懂 —— 钉在这里，改动就变成显式的。
  Check(sizeof(rpc::RpcHeader) == 16, "sizeof(RpcHeader) == 16");
  Check(offsetof(rpc::RpcHeader, type_) == 12, "type_ 在偏移 12");
  Check(rpc::kRpcTypeRequest == 0 && rpc::kRpcTypeResponse == 1,
        "request=0 / response=1");
  // 值初始化（{}）必须把 padding 也清掉 —— Serializer 是裸 memcpy 整个
  // sizeof(RpcHeader)，padding 不清就是把 3 字节栈内容写上网络。
  const rpc::RpcHeader h{};
  const unsigned char* raw = reinterpret_cast<const unsigned char*>(&h);
  Check(raw[13] == 0 && raw[14] == 0 && raw[15] == 0,
        "值初始化的 padding 三字节为 0（否则每帧泄露 3 字节栈内容）");
}

void TestTypeOnWire() {
  std::cout << "[A2] Serializer 写出正确的 type" << std::endl;
  {
    rpc::RpcRequest req = MakeRequest("S", "M", "P", 1);
    std::string buf;
    Check(req.Serializer(buf), "RpcRequest::Serializer 成功");
    Check(static_cast<uint8_t>(buf[12]) == rpc::kRpcTypeRequest,
          "request 的 type 字节是 0");
    Check(buf[13] == 0 && buf[14] == 0 && buf[15] == 0,
          "request 的 padding 三字节为 0");
  }
  {
    rpc::RpcResponse resp = MakeResponse("R", 0, "E", 1);
    std::string buf;
    Check(resp.Serializer(buf), "RpcResponse::Serializer 成功");
    Check(static_cast<uint8_t>(buf[12]) == rpc::kRpcTypeResponse,
          "response 的 type 字节是 1");
  }
}

void TestRoundTrip() {
  std::cout << "[A3] 各自往返" << std::endl;
  {
    rpc::RpcRequest req = MakeRequest("AuthService", "login", "{\"u\":1}", 42);
    std::string buf;
    req.Serializer(buf);
    rpc::RpcRequest out;
    Check(out.Deserializer(buf), "request 反序列化成功");
    Check(out.GetServiceName() == "AuthService", "service_name 一致");
    Check(out.GetMethodName() == "login", "method_name 一致");
    Check(out.GetPayload() == "{\"u\":1}", "payload 一致");
    Check(out.GetSequenceId() == 42, "sequence_id 一致");
  }
  {
    rpc::RpcResponse resp = MakeResponse("{\"uid\":7}", 0, "", 43);
    std::string buf;
    resp.Serializer(buf);
    rpc::RpcResponse out;
    Check(out.Deserializer(buf), "response 反序列化成功");
    Check(out.GetResultData() == "{\"uid\":7}", "result_data 一致");
    Check(out.GetErrorMessage().empty(), "error_message 一致");
    Check(out.GetErrorCode() == 0, "error_code 一致");
    Check(out.GetSequenceId() == 43, "sequence_id 一致");
  }
}

void TestCrossParseRejected() {
  std::cout << "[A4] 交叉解析必须被拒绝" << std::endl;
  {
    rpc::RpcRequest req = MakeRequest("AuthService", "login", "payload", 1);
    std::string buf;
    req.Serializer(buf);
    rpc::RpcResponse out;
    Check(!out.Deserializer(buf),
          "把 request 的字节喂给 RpcResponse::Deserializer → 失败");
  }
  {
    rpc::RpcResponse resp = MakeResponse("result", 0, "err", 1);
    std::string buf;
    resp.Serializer(buf);
    rpc::RpcRequest out;
    Check(!out.Deserializer(buf),
          "把 response 的字节喂给 RpcRequest::Deserializer → 失败");
  }
  {
    // 类型字段被改成未知值时，两个方向都要拒绝。
    rpc::RpcRequest req = MakeRequest("S", "M", "P", 1);
    std::string buf;
    req.Serializer(buf);
    buf[12] = static_cast<char>(7);
    rpc::RpcRequest req_out;
    rpc::RpcResponse resp_out;
    Check(!req_out.Deserializer(buf), "type=7 → RpcRequest 拒绝");
    Check(!resp_out.Deserializer(buf), "type=7 → RpcResponse 拒绝");
  }
}

// 这一条是【反向证明】：如果 Deserializer 不校验 type，会发生什么。
//
// 它故意绕过校验（手工把 type 字节改成 response），好把"静默乱码"演示出来。
// 没有这条，"为什么要加 type 字段"就只是文档里的一句话。
void TestSilentGarbageWithoutCheck() {
  std::cout << "[A5] 反证：不做类型校验就会静默解出乱码" << std::endl;

  rpc::RpcRequest req = MakeRequest("AuthService", "login", "AAAA", 1);
  std::string buf;
  req.Serializer(buf);

  // 只改类型字节，其余一个字节不动 —— 模拟"没有 type 字段"的世界
  buf[12] = static_cast<char>(rpc::kRpcTypeResponse);

  rpc::RpcResponse out;
  const bool ok = out.Deserializer(buf);

  Check(ok, "校验被绕过时，反序列化【成功】返回 true —— 这才是危险之处");
  // request 的 body 和 response 的 body 长得"足够像"：都是长度前缀的字段
  // 串接，所以所有边界检查都过得去，只是每个字段的含义全错位了。
  Check(out.GetResultData() == "AuthService",
        "result_data 读到的其实是 service_name");
  Check(out.GetErrorMessage() == "login",
        "error_message 读到的其实是 method_name");
  Check(out.GetErrorCode() == 4, "error_code 读到的其实是 payload 的长度");
}

// ── B 组：Connect 按类型分派 ──────────────────────────────

void TestDispatchResponse() {
  std::cout << "[B1] response 帧送到 response_callback_" << std::endl;
  int mine = -1;
  int peer = -1;
  if (!MakeTcpPair(mine, peer)) {
    Check(false, "造回环 TCP 连接");
    return;
  }
  auto conn = std::make_shared<rpc::Connect>(mine);

  int req_calls = 0;
  int resp_calls = 0;
  std::string got_result;
  uint32_t got_seq = 0;
  conn->SetRequestCallback(
      [&req_calls](const std::shared_ptr<rpc::Connect>&, rpc::RpcRequest&) {
        ++req_calls;
      });
  conn->SetResponseCallback([&resp_calls, &got_result, &got_seq](
                                const std::shared_ptr<rpc::Connect>&,
                                rpc::RpcResponse& r) {
    ++resp_calls;
    got_result = r.GetResultData();
    got_seq = r.GetSequenceId();
  });

  rpc::RpcResponse resp = MakeResponse("hello", 0, "", 7);
  const std::string wire = EncodeWire(resp);
  Check(SendAll(peer, wire), "把一条 response 帧写到对端");
  Check(PumpUntil(*conn, wire.size(), 500),
        "读到完整的 " + std::to_string(wire.size()) + " 字节");
  Check(conn->ProgressGetMessage(), "ProgressGetMessage 返回 true");

  Check(resp_calls == 1, "response_callback_ 被调用一次");
  Check(req_calls == 0, "request_callback_ 【没有】被调用");
  Check(got_result == "hello", "回调拿到完整解出的 result_data");
  Check(got_seq == 7, "回调拿到正确的 sequence_id");

  ::close(peer);
}

void TestDispatchRequest() {
  std::cout << "[B2] request 帧送到 request_callback_" << std::endl;
  int mine = -1;
  int peer = -1;
  if (!MakeTcpPair(mine, peer)) {
    Check(false, "造回环 TCP 连接");
    return;
  }
  auto conn = std::make_shared<rpc::Connect>(mine);

  int req_calls = 0;
  int resp_calls = 0;
  std::string got_service;
  conn->SetRequestCallback([&req_calls, &got_service](
                               const std::shared_ptr<rpc::Connect>&,
                               rpc::RpcRequest& r) {
    ++req_calls;
    got_service = r.GetServiceName();
  });
  conn->SetResponseCallback(
      [&resp_calls](const std::shared_ptr<rpc::Connect>&, rpc::RpcResponse&) {
        ++resp_calls;
      });

  rpc::RpcRequest req = MakeRequest("ChatService", "send", "{}", 8);
  const std::string wire = EncodeWire(req);
  Check(SendAll(peer, wire), "把一条 request 帧写到对端");
  Check(PumpUntil(*conn, wire.size(), 500),
        "读到完整的 " + std::to_string(wire.size()) + " 字节");
  Check(conn->ProgressGetMessage(), "ProgressGetMessage 返回 true");

  Check(req_calls == 1, "request_callback_ 被调用一次");
  Check(resp_calls == 0, "response_callback_ 【没有】被调用");
  Check(got_service == "ChatService", "回调拿到正确的 service_name");

  ::close(peer);
}

// 推送连接的形状：一条连接上两个方向混走。三条帧一次写进去，靠
// ProgressGetMessage 的循环逐条按类型分派 —— 顺带也复验了粘包。
void TestMixedOnOneConnection() {
  std::cout << "[B3] 同一条连接上 request/response 混走" << std::endl;
  int mine = -1;
  int peer = -1;
  if (!MakeTcpPair(mine, peer)) {
    Check(false, "造回环 TCP 连接");
    return;
  }
  auto conn = std::make_shared<rpc::Connect>(mine);

  std::string order;
  conn->SetRequestCallback([&order](const std::shared_ptr<rpc::Connect>&,
                                    rpc::RpcRequest& r) {
    order += "R" + std::to_string(r.GetSequenceId());
  });
  conn->SetResponseCallback([&order](const std::shared_ptr<rpc::Connect>&,
                                     rpc::RpcResponse& r) {
    order += "S" + std::to_string(r.GetSequenceId());
  });

  // 客户端 bind(request) → 服务端 bind 结果(response) → 服务端推消息(request)
  rpc::RpcRequest bind_req = MakeRequest("PushService", "bind", "{}", 1);
  rpc::RpcResponse bind_resp = MakeResponse("{}", 0, "", 1);
  rpc::RpcRequest push_req = MakeRequest("PushService", "on_message", "{}", 2);

  const std::string wire =
      EncodeWire(bind_req) + EncodeWire(bind_resp) + EncodeWire(push_req);
  Check(SendAll(peer, wire), "一次写入三条帧（request/response/request）");
  Check(PumpUntil(*conn, wire.size(), 500),
        "三条帧全部到达（" + std::to_string(wire.size()) + " 字节）");
  Check(conn->ProgressGetMessage(), "ProgressGetMessage 返回 true");

  Check(order == "R1S1R2", "三条帧按顺序、按各自类型分派（实际：" + order + "）");

  ::close(peer);
}

// 没有注册对应回调时，不能因为一条帧就摘掉整条连接。
void TestNoCallbackKeepsConnection() {
  std::cout << "[B4] 没注册回调 → 丢弃该帧但不关连接" << std::endl;
  int mine = -1;
  int peer = -1;
  if (!MakeTcpPair(mine, peer)) {
    Check(false, "造回环 TCP 连接");
    return;
  }
  auto conn = std::make_shared<rpc::Connect>(mine);
  // 两个回调【都】不注册，模拟服务端收到一条 response（它不该收到）

  rpc::RpcResponse resp = MakeResponse("nobody-home", 0, "", 9);
  const std::string wire = EncodeWire(resp);
  Check(SendAll(peer, wire), "写一条没人处理的 response 帧");
  Check(PumpUntil(*conn, wire.size(), 500), "帧已到达");

  // 返回 false 会让 manager_cycle 调 RemoveConnect 把连接摘掉。
  // 一条格式合法、只是没处理方的帧不该有那个后果。
  Check(conn->ProgressGetMessage(), "ProgressGetMessage 仍返回 true");
  Check(conn->IsRunning(), "连接没有被关闭");

  ::close(peer);
}

// 未知类型走 else 分支，明确报错并返回 false。
void TestUnknownTypeRejected() {
  std::cout << "[B5] 未知类型 → 明确报错，不当成半包干等" << std::endl;
  int mine = -1;
  int peer = -1;
  if (!MakeTcpPair(mine, peer)) {
    Check(false, "造回环 TCP 连接");
    return;
  }
  auto conn = std::make_shared<rpc::Connect>(mine);
  conn->SetRequestCallback(
      [](const std::shared_ptr<rpc::Connect>&, rpc::RpcRequest&) {});
  conn->SetResponseCallback(
      [](const std::shared_ptr<rpc::Connect>&, rpc::RpcResponse&) {});

  // 在【明文】层改类型字节：先造一条 request 帧，解密解压后改掉再重新打包
  rpc::RpcRequest req = MakeRequest("S", "M", "P", 1);
  std::string serialized;
  req.Serializer(serialized);
  serialized[12] = static_cast<char>(99);  // 未知类型
  std::string compressed;
  rpc::ZstdCompress::GetInstance().CompressString(serialized, compressed);
  std::string cipher;
  rpc::AesEncrypt::GetInstance().Encrypt(compressed, cipher);

  const std::string wire = rpc::EncodeFrame(cipher);
  Check(SendAll(peer, wire), "写一条 type=99 的帧");
  Check(PumpUntil(*conn, wire.size(), 500), "帧已到达");
  Check(!conn->ProgressGetMessage(),
        "返回 false —— 未知类型是协议错误，不是'还没读完'");

  ::close(peer);
}

}  // namespace

int main() {
  std::cout << "══════ 协议 type 字段测试 ══════" << std::endl;

  // 加解密要先有密钥：master_key_ 为空时 ShiftEncrypt 里 key[i % 0] 会除零。
  rpc::AesEncrypt::GetInstance().Init("protocol_test_key");

  std::cout << "── A 组：协议层 ──" << std::endl;
  TestLayout();
  TestTypeOnWire();
  TestRoundTrip();
  TestCrossParseRejected();
  TestSilentGarbageWithoutCheck();

  std::cout << "── B 组：Connect 分派 ──" << std::endl;
  TestDispatchResponse();
  TestDispatchRequest();
  TestMixedOnOneConnection();
  TestNoCallbackKeepsConnection();
  TestUnknownTypeRejected();

  std::cout << "════════════════════════════════" << std::endl;
  std::cout << "通过 " << g_pass << "，失败 " << g_fail << std::endl;
  return g_fail == 0 ? 0 : 1;
}
