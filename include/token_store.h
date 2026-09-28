#pragma once

#include <string>

namespace rpc {
namespace token {

// token 默认存活 1 小时。短到能限制泄露窗口，长到不用频繁重登。
inline constexpr int kDefaultTtlSec = 3600;

// 校验结果。三态而非 bool：因为"token 不对"和"后端连不上"的正确处理【相反】——
// 前者是 401（让用户重新登录），后者是 5xx + 告警。
// 压成 bool 就再也分不出来：把"连不上"当成"无效"只是少一次告警，
// 但换个写法当成"没这个 token 所以继续查"就是 fail-open，等于放行任何人。
enum class Verdict {
  kValid,        // 有效，uid_out 是它的持有者
  kInvalid,      // 明确无效：不存在 / 已过期 / 已被 Revoke
  kUnavailable,  // 无法判定：后端不可用（进程内实现永不返回这个值）
};

class Store {
 public:
  virtual ~Store() = default;

  // 签发 token 给 uid，ttl_sec 秒后失效。失败返回空串。
  //
  // ⚠️ 失败必须当【致命】处理：发一个自己都没存进去的 token，等于发一个永远
  //    验不过的 token，用户会看到"登录成功，但下一步就 401"。
  virtual std::string Issue(const std::string& uid, int ttl_sec) = 0;

  // 校验 token，有效则通过 uid_out 返回它属于谁。失败时不动 uid_out。
  //
  // ⚠️ 返回 kValid 只说明"这个 token 有效"，【不】说明"它属于你以为的那个人"。
  //    需要确认归属时（比如推送 bind 要绑到指定 uid），调用方必须自己比对 uid_out。
  virtual Verdict Verify(const std::string& token, std::string& uid_out) = 0;

  // 登出 / 强制下线。幂等：token 不存在不算错误。
  virtual void Revoke(const std::string& token) = 0;
};

}  // namespace token
}  // namespace rpc
