#include <iostream>

#include "service.h"
#include "service_manager.h"

int main() {
  auto& service_manager = rpc::ServiceManager::GetInstance();
  auto user_service = std::make_shared<rpc::UserService>();
  auto rpc_service = std::make_shared<rpc::RpcService>(user_service);
  try {
    service_manager.RegisterService(user_service);
    service_manager.RegisterService(rpc_service);
    std::string args = R"({"message": "Hello, RPC!", "user_id": "42"})";
    std::string res;
    bool success =
        service_manager.HandleRequest("RpcService", "echo", args, res);
    if (success) {
      std::cout << "RPC success: " << res << std::endl;
    } else {
      std::cout << "RPC failed" << std::endl;
    }
  } catch (std::exception& e) {
    std::cout << "RPC failed: " << e.what() << std::endl;
  }
  return 0;
}