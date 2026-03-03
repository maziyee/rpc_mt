#include "zstd_compress.h"

rpc::ZstdCompress& rpc::ZstdCompress::GetInstance() {
  static ZstdCompress instance;
  return instance;
}

bool rpc::ZstdCompress::CompressString(const std::string& input,
                                       std::string& output, Level level) {
  if (input.empty()) {
    output.clear();
    return false;
  }
  size_t max_compressed_size = ZSTD_compressBound(input.size());
  output.resize(max_compressed_size);
  size_t const compressed_size =
      ZSTD_compress(output.data(), output.size(), input.data(), input.size(),
                    static_cast<int>(level));
  if (ZSTD_isError(compressed_size)) {
    return false;
  }
  output.resize(compressed_size);
  return true;
}

bool rpc::ZstdCompress::DecompressString(const std::string& input,
                                         std::string& output) {
  if (input.empty()) {
    output.clear();
    return false;
  }
  size_t decompresssize = ZSTD_getDecompressedSize(input.data(), input.size());
  if (decompresssize == ZSTD_CONTENTSIZE_ERROR) {
    return false;
  }
  if (decompresssize == ZSTD_CONTENTSIZE_UNKNOWN) {
    return false;
  }
  output.resize(decompresssize);
  size_t const decompressed_size =
      ZSTD_decompress(output.data(), output.size(), input.data(), input.size());
  if (ZSTD_isError(decompressed_size)) {
    return false;
  }
  output.resize(decompressed_size);
  return true;
}

bool rpc::ZstdCompress::CompressData(const char* input, size_t input_size,
                                     std::vector<char>& output, Level level) {
  if (!input || input_size == 0) {
    output.clear();
    return false;
  }
  size_t max_compressed_size = ZSTD_compressBound(input_size);
  output.resize(max_compressed_size);
  size_t const compressed_size = ZSTD_compress(
      output.data(), output.size(), input, input_size, static_cast<int>(level));
  if (ZSTD_isError(compressed_size)) {
    return false;
  }
  output.resize(compressed_size);
  return true;
}

bool rpc::ZstdCompress::DecompressData(const char* input, size_t input_size,
                                       std::vector<char>& output) {
  if (!input || input_size == 0) {
    output.clear();
    return false;
  }
  size_t decompresssize = ZSTD_getDecompressedSize(input, input_size);
  if (decompresssize == ZSTD_CONTENTSIZE_ERROR) {
    return false;
  }
  if (decompresssize == ZSTD_CONTENTSIZE_UNKNOWN) {
    return false;
  }
  output.resize(decompresssize);
  size_t const actually_size =
      ZSTD_decompress(output.data(), output.size(), input, input_size);
  if (ZSTD_isError(actually_size)) {
    return false;
  }
  output.resize(actually_size);
  return true;
}
