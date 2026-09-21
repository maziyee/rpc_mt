#pragma once

#include <cstdint>
#include <string>

namespace rpc {

// TCP 分帧头：放在密文【外面】，只解决"从字节流里切出一条消息"。
//
// 为什么需要它：TCP 是字节流，没有消息边界。粘包（两条消息合在一次 recv）
// 和半包（一条消息被拆成多次 recv）都是常态，必须由应用层自己分帧。
//
// 为什么不能复用密文内的 RpcHeader：
//   1) RpcHeader 在密文里 —— 要读它得先解密，可解密又需要知道密文多长，循环依赖
//   2) RpcHeader 里的 body_size_ 描述的是【明文】长度，而线上流动的是
//      压缩 + 加密 + base64 之后的【密文】，中间隔着 zstd 和 base64，两个数毫无关系
//
// 刻意只放两个字段，且都不含业务信息：
//   kMagic     —— 协议识别，读完 8 字节就能拒绝非法帧，不必尝试解密
//   frame_len  —— 密文字节数（TCP 层本来就能观察到包大小，不算新增泄露）
// sequence_id 和明文 body 长度仍留在密文内的 RpcHeader 里，不外泄。
struct FrameHeader {
  static constexpr size_t kSize = 8;
  static constexpr uint32_t kMagic = 0x12345678;

  // 上限是必须的：frame_len 是明文且直接决定"要读多少字节"，
  // 不设限的话对端发一个 0xFFFFFFFF 就能让本端尝试读 4GB（DoS）。
  static constexpr uint32_t kMaxFrameSize = 16 * 1024 * 1024;  // 16MB
};

enum class FrameResult {
  kOk,        // 取出一条完整帧
  kNeedMore,  // 数据不够，等下次读（半包）
  kInvalid,   // 帧头非法：magic 不匹配，或长度越界
};

// 把一条密文封装成帧：[magic(4)][frame_len(4, 网络字节序)][cipher]
std::string EncodeFrame(const std::string& cipher);

// 只看不消费，判断 buf 头部是否已凑够一条完整帧
FrameResult PeekFrameState(const std::string& buf);

// 从 buf 头部取出一条完整密文；成功时把已消费的字节从 buf 中 erase 掉。
// 半包时返回 kNeedMore 且 buf 保持不变 —— 这是与"整包假设"最本质的区别：
// 数据不够是【正常等待】，不是错误。
FrameResult TryDecodeFrame(std::string& buf, std::string& cipher);

}  // namespace rpc
