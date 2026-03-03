#include <zstd.h>

#include <iostream>
#include <string>
#include <vector>

#include "zstd_compress.h"

int main() {
  std::string input = "this is a test string";
  for (int i = 0; i < 10; i++) {
    input += input;
  }
  std::string compressed;
  std::string decompressed;
  if (rpc::ZstdCompress::GetInstance().CompressString(input, compressed)) {
    std::cout << "compressed success" << std::endl;
    std::cout << "compressed size: " << compressed.size() << std::endl;
    std::cout << "original size : " << input.size() << std::endl;
    std::cout << "compression ratio: "
              << 100 * (static_cast<double>(compressed.size()) / input.size())
              << "%" << std::endl;
  } else {
    std::cout << "compressed failed" << std::endl;
  }
  rpc::ZstdCompress::GetInstance().DecompressString(compressed, decompressed);
  if (decompressed == input) {
    std::cout << "decompressed success" << std::endl;
  } else {
    std::cout << "decompressed failed" << std::endl;
  }
  return 0;
}
