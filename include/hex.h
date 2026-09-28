#pragma once

#include <cstddef>
#include <string>

namespace rpc {

// 字节 ↔ 小写 hex。password_hash 和 token_store 都要用，所以放一处 ——
// 两份副本各自演化（比如一份被改成大写）会让格式悄悄不一致。
//
// 只认小写：同一份数据只允许一个合法表示，否则 "AB" 和 "ab" 解析出同一个
// 字节但字符串不相等，做比对 / 缓存键 / 跨系统传输时会持续出问题。

// 任意字节 → 小写 hex 串，长度是字节数的两倍。
inline std::string ToHex(const unsigned char* data, size_t len) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out.push_back(kDigits[data[i] >> 4]);    // 高 4 位
    out.push_back(kDigits[data[i] & 0x0F]);  // 低 4 位
  }
  return out;
}

// 校验：长度必须【精确】等于 expect_len，且只含 0-9a-f。
inline bool IsLowerHex(const std::string& s, size_t expect_len) {
  if (s.size() != expect_len) {
    return false;
  }
  for (const char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

// 小写 hex → 字节。
// ⚠️ 本函数【不】校验 out 的容量，也【不】校验长度是否为偶数 ——
//    调用方必须先用 IsLowerHex 校验过（它同时保证了两者）。
inline bool FromHex(const std::string& hex, unsigned char* out) {
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    return -1;  // 非法返回 -1，不是 0 —— 否则 'z' 会被静默当成 0x0
  };
  for (size_t i = 0; i < hex.size(); i += 2) {
    const int hi = nibble(hex[i]);
    const int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    out[i / 2] = static_cast<unsigned char>((hi << 4) | lo);
  }
  return true;
}

}  // namespace rpc
