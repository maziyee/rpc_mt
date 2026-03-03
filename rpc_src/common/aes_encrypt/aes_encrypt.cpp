#include "aes_encrypt.h"

#include <algorithm>
#include <random>
#include <stdexcept>

#include "log_manager.h"

namespace rpc {
namespace {
const std::string BASE_64_CHARS =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

bool AesEncrypt::Encrypt(const std::string& data, std::string& ciphertext) {
  if (data.empty()) {
    LOG_ERROR("the encrypt data should not be empty");
    return false;
  }
  try {
    std::string random_key = this->GenerateRandomKey(kKEY_SIZE);
    std::string random_encrypted = this->ShiftEncrypt(data, random_key);
    std::string master_encrypted =
        this->ShiftEncrypt(random_key, this->master_key_);
    std::string combined = master_encrypted + random_encrypted;
    ciphertext = this->Base64Encode(combined);
    return true;
  } catch (std::exception& e) {
    LOG_ERROR("Encrypt failed , {}", e.what());
    return false;
  }
}

bool AesEncrypt::Decrypt(const std::string& encrypted_data,
                         std::string& plaintext) {
  try {
    std::string denode_data = this->Base64Decode(encrypted_data);
    if (denode_data.size() <= kKEY_SIZE) {
      LOG_ERROR("the denote data is too short");
      return false;
    }
    std::string encrypt_key = denode_data.substr(0, kKEY_SIZE);
    std::string encrypt_part = denode_data.substr(kKEY_SIZE);
    std::string decrypt_random_key =
        this->ShiftDecrypt(encrypt_key, master_key_);
    plaintext = this->ShiftDecrypt(encrypt_part, decrypt_random_key);
    return true;
  } catch (std::exception& e) {
    LOG_ERROR("the decrypt failed {}", e.what());
    return false;
  }
}

std::string AesEncrypt::Base64Encode(const std::string& data) {
  std::string res;
  int i = 0;
  int j = 0;
  unsigned char char_array_3[3];
  unsigned char char_array_4[4];
  const unsigned char* byte_encode =
      reinterpret_cast<const unsigned char*>(data.data());
  size_t data_len = data.size();
  while (data_len--) {
    char_array_3[i++] = *(byte_encode++);
    if (i == 3) {
      char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
      char_array_4[1] =
          ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
      char_array_4[2] =
          ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
      char_array_4[3] = char_array_3[2] & 0x3f;
      for (i = 0; (i < 4); i++) {
        res += BASE_64_CHARS[char_array_4[i]];
      }
      i = 0;
    }
  }
  if (i) {
    for (j = i; j < 3; j++) {
      char_array_3[j] = '\0';
    }
    char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
    char_array_4[1] =
        ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
    char_array_4[2] =
        ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
    char_array_4[3] = char_array_3[2] & 0x3f;
    for (j = 0; (j < i + 1); j++) {
      res += BASE_64_CHARS[char_array_4[j]];
    }
    while ((i++ < 3)) {
      res += '=';
    }
  }
  return res;
}

std::string AesEncrypt::Base64Decode(const std::string& data) {
  std::string res;
  std::vector<int> base64_index(256, -1);
  for (int i = 0; i < BASE_64_CHARS.size(); i++) {
    base64_index[BASE_64_CHARS[i]] = i;
  }

  size_t data_len = data.size();
  int i = 0;
  int j = 0;
  int in_ = 0;
  unsigned char char_array_4[4], char_array_3[3];
  while (data_len-- && data[in_] != '=') {
    char_array_4[i++] = data[in_];
    in_++;
    if (i == 4) {
      for (i = 0; i < 4; i++) {
        char_array_4[i] = base64_index[char_array_4[i]];
      }
      char_array_3[0] =
          (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
      char_array_3[1] =
          ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
      char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
      for (i = 0; (i < 3); i++) {
        res += char_array_3[i];
      }
      i = 0;
    }
  }
  if (i) {
    for (j = i; j < 4; j++) {
      char_array_4[j] = 0;
    }
    for (j = 0; j < 4; j++) {
      char_array_4[j] = base64_index[char_array_4[j]];
    }
    char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
    char_array_3[1] =
        ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
    char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
    for (j = 0; j < i - 1; j++) {
      res += char_array_3[j];
    }
  }
  return res;
}

std::string AesEncrypt::GenerateRandomKey(size_t key_size) {
  std::string res;
  res.reserve(key_size);
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 255);
  for (int i = 0; i < key_size; i++) {
    res.push_back(static_cast<char>(dis(gen)));
  }
  return res;
}

std::string AesEncrypt::ShiftEncrypt(const std::string& data,
                                     const std::string& key) {
  std::string res;
  res.reserve(data.size());
  for (int i = 0; i < data.size(); i++) {
    unsigned char c = data[i];
    unsigned char k = key[i % key.size()];
    unsigned char p = (i % 256);
    res.push_back(static_cast<char>((c + k + p) % 256));
  }
  return res;
}

std::string AesEncrypt::ShiftDecrypt(const std::string& data,
                                     const std::string& key) {
  std::string res;
  res.reserve(data.size());
  for (int i = 0; i < data.size(); i++) {
    unsigned char c = data[i];
    unsigned char k = key[i % key.size()];
    unsigned char p = (i % 256);
    res.push_back(static_cast<char>((c - k - p + 512) % 256));
  }
  return res;
}

}  // namespace rpc
