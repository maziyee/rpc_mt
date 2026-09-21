#include "frame_codec.h"

#include <arpa/inet.h>

#include <cstring>

namespace rpc {
namespace {
constexpr size_t kMagicOffset = 0;
constexpr size_t kLenOffset = sizeof(uint32_t);
}  // namespace

std::string EncodeFrame(const std::string& cipher) {
  std::string frame;
  frame.reserve(FrameHeader::kSize + cipher.size());

  const uint32_t magic = FrameHeader::kMagic;
  const uint32_t net_len = htonl(static_cast<uint32_t>(cipher.size()));

  frame.append(reinterpret_cast<const char*>(&magic), sizeof(magic));
  frame.append(reinterpret_cast<const char*>(&net_len), sizeof(net_len));
  frame.append(cipher);
  return frame;
}

FrameResult PeekFrameState(const std::string& buf) {
  if (buf.size() < FrameHeader::kSize) {
    return FrameResult::kNeedMore;
  }

  uint32_t magic = 0;
  uint32_t net_len = 0;
  std::memcpy(&magic, buf.data() + kMagicOffset, sizeof(magic));
  std::memcpy(&net_len, buf.data() + kLenOffset, sizeof(net_len));

  if (magic != FrameHeader::kMagic) {
    return FrameResult::kInvalid;
  }

  const uint32_t frame_len = ntohl(net_len);
  if (frame_len == 0 || frame_len > FrameHeader::kMaxFrameSize) {
    return FrameResult::kInvalid;
  }
  if (buf.size() < FrameHeader::kSize + frame_len) {
    return FrameResult::kNeedMore;
  }
  return FrameResult::kOk;
}

FrameResult TryDecodeFrame(std::string& buf, std::string& cipher) {
  const FrameResult state = PeekFrameState(buf);
  if (state != FrameResult::kOk) {
    return state;  // kNeedMore 时 buf 保持原样，等下次读到更多
  }

  uint32_t net_len = 0;
  std::memcpy(&net_len, buf.data() + kLenOffset, sizeof(net_len));
  const uint32_t frame_len = ntohl(net_len);

  cipher.assign(buf.data() + FrameHeader::kSize, frame_len);
  // 只消费掉这一条，剩下的留在 buf 里 —— 粘包由此自然解决
  buf.erase(0, FrameHeader::kSize + frame_len);
  return FrameResult::kOk;
}

}  // namespace rpc
