#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "log_manager.h"

namespace rpc {
class LoadBanlance {
 public:
  virtual ~LoadBanlance() = default;
  virtual std::string Select(std::vector<std::string>& servers,
                             std::string& client) = 0;

  static std::shared_ptr<LoadBanlance> GetInstance();

  static bool InitLoadBanlance(const std::string& type = "random");

  static std::string SelectServers(std::vector<std::string>& servers,
                                   std::string& client);

 protected:
  LoadBanlance() noexcept = default;

 private:
  LoadBanlance(LoadBanlance&) = delete;
  LoadBanlance& operator=(LoadBanlance&) = delete;
  LoadBanlance(LoadBanlance&&) = delete;
  LoadBanlance& operator=(LoadBanlance&&) = delete;

 private:
  static std::shared_ptr<LoadBanlance> instance_;
  static std::mutex mut_;
};
}  // namespace rpc