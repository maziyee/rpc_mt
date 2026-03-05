#pragma once

#include <memory>
#include <string>
#include <vector>

namespace rpc {
class AesEncrypt {
 public:
  static AesEncrypt& GetInstance() {
    static AesEncrypt instance;
    return instance;
  };
  bool Encrypt(const std::string& data, std::string& ciphertext);
  bool Decrypt(const std::string& encrypted_data, std::string& plaintext);
  void Init(const std::string mas_key) { this->master_key_ = mas_key; }

  AesEncrypt(const AesEncrypt&) = delete;
  AesEncrypt& operator=(const AesEncrypt&) = delete;

 private:
  ~AesEncrypt() = default;
  AesEncrypt() = default;
  std::string Base64Encode(const std::string& data);
  std::string Base64Decode(const std::string& data);
  std::string GenerateRandomKey(size_t key_size);
  std::string ShiftEncrypt(const std::string& data, const std::string& key);
  std::string ShiftDecrypt(const std::string& data, const std::string& key);

 private:
  std::string master_key_;
  static constexpr int kKEY_SIZE = 32;
  static constexpr int kBLOCK_SIZE = 16;
};
}  // namespace rpc