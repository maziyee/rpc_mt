// 网络层 5 个修复的测试。一条用例对应一个修复。
//
//   ./build/network_test
//
//   ① connect.cpp        ::send 加 MSG_NOSIGNAL（否则 SIGPIPE 杀进程）
//   ② connect.h/.cpp     has_pending_ + FlushSendBuf（残留字节可重发）
//      manager_cycle.cpp  HandleEvent 的 EPOLLOUT 分支
//   ③ connect_manage.cpp CloseAll 用 swap 遍历（迭代器失效）
//   ④ rpc_client.cpp     Connect() 失败不再谎报成功（空指针）
//   ⑤ manager_cycle.cpp  EPOLLHUP 走 RemoveConnect 而不是 Remove
//
// ①②③⑤ 零外部依赖。④ 需要 ZK 起着【且 server 未启动】—— 那一条里有说明。

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "aes_encrypt.h"
#include "connect.h"
#include "connect_manage.h"
#include "log_manager.h"
#include "manager_cycle.h"
#include "rpc_client.h"
#include "rpc_protobuf.h"
#include "serializer_mananger.h"
#include "spdlog_config.h"
#include "zk_config.h"
#include "zk_handler.h"

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

void Skip(const std::string& why) {
  std::cout << "  ⚠️  跳过：" << why << std::endl;
}

// 造一对已连接的回环 TCP socket。mine 是"我们的"一端，peer 是对端。
//
// 用真 TCP 而不是 socketpair(AF_UNIX)：Connect 的构造里会 getpeername +
// 按 AF_INET 解析地址，AF_UNIX 的地址结构对不上。
//
// mine 设成非阻塞 —— 缓冲满时要的是 EAGAIN，阻塞 socket 会直接卡住。
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

// ZK 端口通不通。
//
// 为什么不能靠 InitZkHandler 的返回值判断：ZK 是【异步】连接，InitZkHandler
// 会立刻返回 true，然后在后台线程里一直重试。等后面某个 zoo_* 调用时才发现
// 连不上 —— 而那个调用会一直卡着不返回（表现就是测试挂死）。
//
// 所以先用一次普通 TCP connect 探一下。ZK 没起时本机 connect 会立刻
// ECONNREFUSED（不会等超时），所以这里不需要额外设超时。
bool ZkPortOpen(const std::string& host, int port) {
  // 用 getaddrinfo 而不是 inet_pton：配置里写的是 "localhost"，
  // 而 inet_pton 只解析数字地址、不查名字，对它一律返回失败。
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) !=
      0) {
    return false;
  }
  bool ok = false;
  for (addrinfo* p = res; p != nullptr && !ok; p = p->ai_next) {
    const int fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) {
      continue;
    }
    ok = ::connect(fd, p->ai_addr, p->ai_addrlen) == 0;
    ::close(fd);
  }
  ::freeaddrinfo(res);
  return ok;
}

// 造一个带 payload 的响应，好让 Connect::Write 有东西可发
rpc::RpcResponse MakeResponse(const std::string& payload) {
  rpc::RpcResponse resp;
  resp.SetSequenceId(1);
  resp.SetErrorCode(0);
  resp.SetErrorMessage("ok");
  resp.SetResultData(payload);
  return resp;
}

}  // namespace

int main() {
  std::cout << "══════ 网络层修复测试 ══════" << std::endl;

  // Write 路径会走 AES，不 Init 的话 master_key_ 是空串 →
  // ShiftEncrypt 里 key[i % 0] 直接除零（SIGFPE）。所以必须先初始化。
  rpc::AesEncrypt::GetInstance().Init("network_test_key");

  // ── ① MSG_NOSIGNAL ──
  std::cout << "[1] MSG_NOSIGNAL：往已断开的连接写，不能杀进程" << std::endl;
  {
    int mine = -1;
    int peer = -1;
    Check(MakeTcpPair(mine, peer), "造一对回环 TCP 连接");

    // SO_LINGER{l_onoff=1, l_linger=0} 让 close 发 RST 而不是 FIN ——
    // 于是我们的下一次 send 立刻拿到 EPIPE。不这么做的话是"第一次 send
    // 成功、第二次才炸"，一条用例可能撞不上。
    linger lg{};
    lg.l_onoff = 1;
    lg.l_linger = 0;
    ::setsockopt(peer, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    ::close(peer);
    ::usleep(50 * 1000);  // 等 RST 到达

    rpc::Connect conn(mine);
    // Write 收非 const 引用（Serializer 是非 const 虚函数），所以不能传临时量
    rpc::RpcResponse resp = MakeResponse(std::string(64, 'y'));
    const bool ok = conn.Write(resp);

    // ⚠️ 如果 MSG_NOSIGNAL 没加，上面那行会触发 SIGPIPE，整个测试进程当场
    //    死掉 —— 后面一个 Check 都打印不出来。所以"还能走到这里"本身就是断言。
    Check(!ok, "写失败并返回 false");
    Check(true, "进程仍然活着（没处理 SIGPIPE 的话根本到不了这一行）");
  }

  // ── ② 残留字节保留 + FlushSendBuf ──
  std::cout << "[2] 发送缓冲满：残留被保留，且能重发" << std::endl;
  {
    int mine = -1;
    int peer = -1;
    Check(MakeTcpPair(mine, peer), "造一对回环 TCP 连接");

    int sndbuf = 4096;  // 调小，好让它快点满
    ::setsockopt(mine, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    rpc::Connect conn(mine);

    // 1 MB 近似不可压缩的数据。用全 'x' 的话 zstd 会把它压成几百字节、
    // 根本填不满缓冲，这条用例就白测了。
    std::string payload(1 << 20, '\0');
    uint32_t x = 12345;
    for (size_t i = 0; i < payload.size(); ++i) {
      x = x * 1664525u + 1013904223u;  // 只求不可压缩，不求密码学强度
      payload[i] = static_cast<char>(x >> 24);
    }

    rpc::RpcResponse big = MakeResponse(payload);
    const bool ok = conn.Write(big);
    Check(ok, "Write 返回 true —— EAGAIN 不算错误（这正是危险之处）");
    Check(conn.HasPendingSend(), "残留字节被保留（has_pending_ 为真）");

    // 对端把数据读走，腾出接收窗口。回环上一个往返就够，但留点余量。
    char drain[65536];
    bool flushed = false;
    for (int i = 0; i < 200 && !flushed; ++i) {
      while (::recv(peer, drain, sizeof(drain), MSG_DONTWAIT) > 0) {
      }
      conn.FlushSendBuf();
      flushed = !conn.HasPendingSend();
      if (!flushed) {
        ::usleep(2 * 1000);
      }
    }
    Check(flushed, "对端腾出空间后，FlushSendBuf 把残留发完");

    ::close(peer);
  }

  // ── ③ CloseAll：边走边删 ──
  std::cout << "[3] CloseAll：回调里删元素不会让迭代器失效" << std::endl;
  {
    auto& mgr = rpc::ConnectManage::GetInstance();
    mgr.CloseAll();  // 清掉上一个用例可能留下的

    std::vector<int> peers;
    for (int i = 0; i < 8; ++i) {
      int mine = -1;
      int peer = -1;
      if (!MakeTcpPair(mine, peer)) {
        Check(false, "造连接失败");
        break;
      }
      auto c = std::make_shared<rpc::Connect>(mine);
      // 复现 ManagerCycle 注册的那个 close 回调 —— 它一路走到 RemoveConnect，
      // 也就是"在遍历 m_connects 的过程中删 m_connects 里的元素"。
      c->SetCloseCallback([&mgr](const std::shared_ptr<rpc::Connect>& conn) {
        mgr.RemoveConnect(conn->GetFd());
      });
      mgr.AddConnect(c);
      peers.push_back(peer);
    }
    Check(mgr.GetConnectCount() == 8, "8 条连接都登记了");

    mgr.CloseAll();  // ← 修复前：迭代器在这里失效
    Check(mgr.GetConnectCount() == 0, "CloseAll 之后连接表清空");
    Check(true, "遍历途中回调删元素，没有崩溃");

    for (const int fd : peers) {
      ::close(fd);
    }
  }

  // ── ④ Connect() 失败不能谎报成功 ──
  std::cout << "[4] RpcClient::Connect 失败时返回 false（不炸在 Call 里）"
            << std::endl;
  {
    std::filesystem::path exe_dir =
        std::filesystem::canonical("/proc/self/exe").parent_path();
    std::filesystem::path config_dir = exe_dir / "../config";

    rpc::SpdlogConfig spdlog_config;
    spdlog_config.InitSpdlog((config_dir / "spdlog_config.json").string());
    rpc::Logger::GetInstance().Init(&spdlog_config);

    rpc::ZkConfig zk_config;
    zk_config.InitZkConfig((config_dir / "zk_config.json").string());

    // ⚠️ 探测必须在 InitZkHandler 【之前】。ZK 是异步连接的：初始化会返回
    //    成功，然后在后台一直重试，直到后面某个 zoo_* 调用才卡住 ——
    //    那样测试就是挂死，而不是优雅跳过。
    if (!ZkPortOpen(zk_config.GetHost(), zk_config.GetPort())) {
      Skip("ZooKeeper 没起（" + zk_config.GetHost() + ":" +
           std::to_string(zk_config.GetPort()) + "）—— 这条用例需要它");
    } else if (!rpc::ZkHandler::GetInstance().InitZkHandler(&zk_config)) {
      Skip("InitZkHandler 失败");
    } else {
      // 注册一个指向【没人监听】的端口的节点，制造"注册了但连不上"的场景。
      // 临时节点 —— 本进程退出、ZK 会话结束时自动消失，不会污染命名空间。
      rpc::ZkHandler::GetInstance().RegistryANode("network_test_dead",
                                                  "127.0.0.1:1");
      const auto servers = rpc::ZkHandler::GetInstance().GetServers();
      if (servers.size() > 1) {
        // 有别的节点时 GetServer 可能选中真节点，这条用例失去意义。
        // 不误报成失败 —— 但也不假装通过。
        Skip("ZK 里还有别的节点（" + std::to_string(servers.size()) +
             " 个），无法保证选到死地址。这条用例要求 server 未启动。");
      } else {
        rpc::RpcClient client((config_dir / "client_config.json").string(),
                              (config_dir / "zk_config.json").string());
        const bool ok = client.Connect();
        Check(!ok, "连不上时 Connect() 返回 false");
        Check(!client.IsConnect(), "IsConnect() 也是 false");

        // 关键断言：修复前 is_connected_ 被谎报成 true，这一句会在
        // SendRequest 里 this->conn_->Write(...) 空指针解引用、直接段错误。
        nlohmann::json args;
        args["message"] = "x";
        nlohmann::json res;
        const bool called = client.Call<nlohmann::json, nlohmann::json>(
            "RpcService", "echo", rpc::SerializerType::JSON, args, res);
        Check(!called, "随后的 Call() 返回 false，而不是段错误");
      }
    }
  }

  // ── ⑤ EPOLLHUP 要走 RemoveConnect ──
  std::cout << "[5] EPOLLHUP：连接从 epoll 和连接表【都】摘掉" << std::endl;
  {
    auto& mgr = rpc::ConnectManage::GetInstance();
    mgr.CloseAll();

    int mine = -1;
    int peer = -1;
    Check(MakeTcpPair(mine, peer), "造一对回环 TCP 连接");

    mgr.AddConnect(std::make_shared<rpc::Connect>(mine));
    rpc::ManagerCycle cycle(&mgr);
    Check(mgr.GetConnect(mine) != nullptr, "摘之前连接在表里");

    cycle.HandleEvent(mine, EPOLLHUP);  // ← 修复前走 Remove()，只摘 epoll

    Check(mgr.GetConnect(mine) == nullptr,
          "HandleEvent(EPOLLHUP) 之后连接已从连接表摘掉");
    ::close(peer);
  }

  std::cout << "════════════════════════════════" << std::endl;
  std::cout << "通过 " << g_pass << "，失败 " << g_fail << std::endl;
  return g_fail == 0 ? 0 : 1;
}
