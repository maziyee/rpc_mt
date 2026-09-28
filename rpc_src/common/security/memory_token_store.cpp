#include "memory_token_store.h"

#include <openssl/rand.h>

#include <chrono>
#include <string>

#include "hex.h"

namespace rpc {
namespace token {
namespace {

// 32 字节 = 256 位随机。够长，不存在被猜出来的可能。
constexpr size_t kTokenBytes = 32;

// Verify 也会顺手清理过期条目，但最多每这么久一次 ——
// 每个请求都全表扫描是 O(n)，高 QPS 下 n 可能不小。
constexpr int kPurgeIntervalSec = 60;

}  // namespace

std::string MemoryStore::Issue(const std::string& uid, int ttl_sec) {
  if (uid.empty() || ttl_sec <= 0) {
    return "";
  }

  // RAND_bytes 是 CSPRNG。token 绝不能用 rand()/mt19937 生成 ——
  // 可预测的 token 可以被离线枚举出来，等于把所有人的账号送出去。
  unsigned char raw[kTokenBytes];
  if (RAND_bytes(raw, static_cast<int>(sizeof(raw))) != 1) {
    return "";  // 熵源不可用，极罕见
  }
  const std::string token = ToHex(raw, sizeof(raw));

  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(this->mutex_);
    this->PurgeLocked(now);
    this->tokens_[token] =
        Entry{uid, now + std::chrono::seconds(ttl_sec)};
  }
  return token;
}

Verdict MemoryStore::Verify(const std::string& token, std::string& uid_out) {
  if (token.empty()) {
    return Verdict::kInvalid;
  }

  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(this->mutex_);

  // 限频清理：不像 Issue 那样每次都全表扫，但也不让过期条目无限堆积。
  if (now - this->last_purge_ >= std::chrono::seconds(kPurgeIntervalSec)) {
    this->PurgeLocked(now);
  }

  const auto it = this->tokens_.find(token);
  if (it == this->tokens_.end()) {
    return Verdict::kInvalid;
  }
  if (it->second.expires_at <= now) {
    return Verdict::kInvalid;  // 已过期；条目本身留给 PurgeLocked 删
  }

  uid_out = it->second.uid;
  return Verdict::kValid;
}

void MemoryStore::Revoke(const std::string& token) {
  // 幂等：token 不存在时 erase 返回 0，不算错误 —— 重复登出是正常操作。
  std::lock_guard<std::mutex> lock(this->mutex_);
  this->tokens_.erase(token);
}

std::size_t MemoryStore::Size() const {
  std::lock_guard<std::mutex> lock(this->mutex_);
  return this->tokens_.size();
}

void MemoryStore::PurgeLocked(std::chrono::steady_clock::time_point now) {
  for (auto it = this->tokens_.begin(); it != this->tokens_.end();) {
    if (it->second.expires_at <= now) {
      it = this->tokens_.erase(it);  // erase 返回下一个位置
    } else {
      ++it;
    }
  }
  this->last_purge_ = now;
}

}  // namespace token
}  // namespace rpc
