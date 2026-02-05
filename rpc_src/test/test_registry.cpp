#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "log_manager.h"
#include "service_registry.h"

int main() {
  try {
    ServiceRegistry registry("127.0.0.1:2181");
    if (!registry.IsConnected()) {
      std::cerr << "registry not connected" << std::endl;
      return -1;
    }
    const std::string& service_name = "test_service";
    const std::string& service_addr = "127.0.0.1:8080";
    if (!registry.Register(service_name, service_addr)) {
      std::cerr << "registry failed" << std::endl;
      return -1;
    }

    std::cout << "CONTINUE" << std::endl;
    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (!registry.IsConnected()) {
        std::cerr << "registry lost connected" << std::endl;
        return -1;
      }
    }
  } catch (std::exception& e) {
    std::cerr << "main failed: " << e.what() << std::endl;
    return -1;
  } catch (...) {
    std::cerr << "main unknown failed" << std::endl;
    return -1;
  }
};