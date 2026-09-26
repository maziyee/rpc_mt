#pragma once

#include <string>

namespace rpc {

// PBKDF2-HMAC-SHA256 密码哈希。
//
// 输出是【自描述】编码串，算法、迭代数、盐全部随行存储：
//   pbkdf2_sha256$<iterations>$<salt_hex(32字符)>$<dk_hex(64字符)>
//
// 迭代数随行的意义：将来调高它时，存量用户仍能用自己那一行的参数校验通过，
// 不需要任何数据迁移。拆成独立的列则有"更新两个忘第三个"的部分更新风险，
// 而这三个值必须同时有效。
//
// 盐用 hex 不用 base64：base64 的 + / = 在转义、日志、URL 场景里会咬人。
//
// 本模块不抛异常、不打日志 —— 失败靠返回值表达，由调用方记日志。
// 这样它只依赖 OpenSSL，password_test 能独立编译（约 2 秒），不必拖进 spdlog。
namespace password {

// 默认迭代数。压测时可以调低，但不要为了 QPS 数字降到安全线以下。
inline constexpr int kDefaultIterations = 100000;

// iterations 的上界。Verify 的输入可能来自数据库、将来也可能来自别处，
// 不设上界的话一个超大的迭代数就是一次 CPU 耗尽攻击。
inline constexpr int kMaxIterations = 10000000;

// 生成编码串。失败（参数非法 / 熵源不可用）返回空串。
std::string Hash(const std::string& password,
                 int iterations = kDefaultIterations);

// 校验。encoded 格式非法、或密码不匹配，都返回 false。
bool Verify(const std::string& password, const std::string& encoded);

}  // namespace password
}  // namespace rpc
