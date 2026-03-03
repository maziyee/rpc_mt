#pragma once

#include <zstd.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rpc {
enum class Level {
  kFASTEST = 1,
  kDEFAULT = 3,
  kBETTER = 7,
  kBEST = 19,
  kMAX = 22
};
class ZstdCompress {
 public:
  static ZstdCompress& GetInstance();
  bool CompressString(const std::string& input, std::string& output,
                      Level level = Level::kDEFAULT);
  bool DecompressString(const std::string& input, std::string& output);
  bool CompressData(const char* input, size_t input_size,
                    std::vector<char>& output, Level level = Level::kDEFAULT);
  bool DecompressData(const char* input, size_t input_size,
                      std::vector<char>& output);
  ZstdCompress(const ZstdCompress&) = delete;
  ZstdCompress& operator=(const ZstdCompress&) = delete;
  ZstdCompress(ZstdCompress&&) = delete;
  ZstdCompress& operator=(ZstdCompress&&) = delete;
  ~ZstdCompress() = default;

 private:
  ZstdCompress() = default;

 private:
};
}  // namespace rpc