#include "password_hash.h"

#include "hex.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstddef>

namespace rpc {
namespace password {
namespace {

// 盐 16 字节、派生密钥 32 字节；hex 表示所以字符数翻倍
constexpr size_t kSaltBytes = 16;
constexpr size_t kKeyBytes = 32;
constexpr size_t kSaltHexLen = kSaltBytes * 2;  // 32
constexpr size_t kKeyHexLen = kKeyBytes * 2;    // 64

constexpr char kAlgorithm[] = "pbkdf2_sha256";

// hex 编解码在 hex.h —— token_store 也要用，放一处避免两份副本漂移。

bool Derive(const std::string& password, const unsigned char* salt,
            size_t salt_len, int iterations, unsigned char* out,
            size_t out_len) {
  // password.c_str() + 显式长度：密码里混进 \0 也能正确参与
  return PKCS5_PBKDF2_HMAC(password.c_str(),
                           static_cast<int>(password.size()), salt,
                           static_cast<int>(salt_len), iterations,
                           EVP_sha256(), static_cast<int>(out_len),
                           out) == 1;
}

}  // namespace

std::string Hash(const std::string& password, int iterations) {
  if (iterations < 1 || iterations > kMaxIterations) {
    return "";
  }

  // RAND_bytes 是 CSPRNG。盐绝不能用 rand()/mt19937 —— 可预测的盐等于没有盐。
  unsigned char salt[kSaltBytes];
  if (RAND_bytes(salt, static_cast<int>(sizeof(salt))) != 1) {
    return "";  // 熵源不可用，极罕见
  }

  unsigned char key[kKeyBytes];
  if (!Derive(password, salt, sizeof(salt), iterations, key, sizeof(key))) {
    return "";
  }

  std::string out;
  out.reserve(sizeof(kAlgorithm) - 1 + 1 + 8 + 1 + kSaltHexLen + 1 +
              kKeyHexLen);
  out += kAlgorithm;
  out += '$';
  out += std::to_string(iterations);
  out += '$';
  out += ToHex(salt, sizeof(salt));
  out += '$';
  out += ToHex(key, sizeof(key));
  return out;
}

bool Verify(const std::string& password, const std::string& encoded) {
  // 拆成 算法 $ 迭代数 $ 盐 $ 派生密钥
  const size_t p1 = encoded.find('$');
  if (p1 == std::string::npos) {
    return false;
  }
  const size_t p2 = encoded.find('$', p1 + 1);
  if (p2 == std::string::npos) {
    return false;
  }
  const size_t p3 = encoded.find('$', p2 + 1);
  if (p3 == std::string::npos) {
    return false;
  }
  if (encoded.find('$', p3 + 1) != std::string::npos) {
    return false;  // 段数多了，串是坏的
  }

  if (encoded.compare(0, p1, kAlgorithm) != 0) {
    return false;
  }

  const std::string iter_str = encoded.substr(p1 + 1, p2 - p1 - 1);
  const std::string salt_hex = encoded.substr(p2 + 1, p3 - p2 - 1);
  const std::string key_hex = encoded.substr(p3 + 1);

  if (!IsLowerHex(salt_hex, kSaltHexLen) ||
      !IsLowerHex(key_hex, kKeyHexLen)) {
    return false;
  }

  // 逐位累加而不是 std::stoll：不抛异常，而且能边加边判上界（不会溢出）
  if (iter_str.empty() || iter_str.size() > 9) {
    return false;
  }
  long long iterations = 0;
  for (const char c : iter_str) {
    if (c < '0' || c > '9') {
      return false;
    }
    iterations = iterations * 10 + (c - '0');
    if (iterations > kMaxIterations) {
      return false;
    }
  }
  if (iterations < 1) {
    return false;
  }

  unsigned char salt[kSaltBytes];
  unsigned char expected[kKeyBytes];
  if (!FromHex(salt_hex, salt) || !FromHex(key_hex, expected)) {
    return false;
  }

  unsigned char actual[kKeyBytes];
  if (!Derive(password, salt, sizeof(salt),
             static_cast<int>(iterations), actual, sizeof(actual))) {
    return false;
  }

  // 用 CRYPTO_memcmp 而不是 ==：普通比较在第一个不同的字节处提前返回，
  // 耗时会泄露"前面有几个字节对上了"。
  // 本场景下攻击者无法控制派生密钥的前缀（那是哈希输出），所以这防的不是
  // 一个现实的攻击 —— 但它是密码学代码的惯例，成本为零，也保护将来在这里
  // 加 token 比对之类的代码。
  return CRYPTO_memcmp(actual, expected, sizeof(actual)) == 0;
}

}  // namespace password
}  // namespace rpc
