#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

#include "token_store.h"

namespace rpc {
namespace token {

// 进程内的 token 存储。
//
// 单实例够用。多实例时【必须】换成共享存储（将来的 RedisStore）—— 多进程
// 内存不共享：用户在节点 1 登录、请求被路由到节点 2 时会验不过。
// 换的时候 AuthService 不用改，因为它是对着 Store 的三态返回值写的。
//
// 线程安全：内部有 mutex。AuthService 跑在线程池 worker 上，会被并发调用。
class MemoryStore : public Store {
 public:
  MemoryStore() = default;
  ~MemoryStore() override = default;

  std::string Issue(const std::string& uid, int ttl_sec) override;
  Verdict Verify(const std::string& token, std::string& uid_out) override;
  void Revoke(const std::string& token) override;

  // 当前条目数（含尚未清理的过期条目）。给测试和监控用。
  std::size_t Size() const;

  MemoryStore(const MemoryStore&) = delete;
  MemoryStore& operator=(const MemoryStore&) = delete;
  MemoryStore(MemoryStore&&) = delete;
  MemoryStore& operator=(MemoryStore&&) = delete;

 private:
  struct Entry {
    std::string uid;
    std::chrono::steady_clock::time_point expires_at;
  };

  // 清掉已过期的条目。只判断不清理的话，map 就是只增不减的（过期条目永远
  // 留在里面）—— 登录量大的服务跑几周就会看出内存涨。
  // 调用方必须已持有 mutex_。
  void PurgeLocked(std::chrono::steady_clock::time_point now);

  mutable std::mutex mutex_;
  std::unordered_map<std::string, Entry> tokens_;  // token → {uid, 到期时刻}
  std::chrono::steady_clock::time_point last_purge_{};
};

}  // namespace token
}  // namespace rpc
