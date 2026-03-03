#include <iostream>
#include <vector>

#include "aes_encrypt.h"

int main() {
  auto& encryptor = rpc::AesEncrypt::GetInstance();
  std::string data = "hello world";

  std::string ciphertext;
  encryptor.Encrypt(data, ciphertext);
  std::cout << "ciphertext: " << ciphertext << std::endl;
  std::string plaintext;
  encryptor.Decrypt(ciphertext, plaintext);
  std::cout << "plaintext: " << plaintext << std::endl;
  if (plaintext == data) {
    std::cout << "the encrypt test sucess" << std::endl;
  } else {
    std::cout << "the encrtpt test failed" << std::endl;
  }
  return 0;
}
