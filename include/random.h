#pragma once

#include <random>

#include "load_balance.h"
#include "log_manager.h"

namespace rpc {
class RandomLoad : public LoadBanlance {
 public:
  RandomLoad() : rd_(), gen_(rd_()){};
  std::string Select(std::vector<std::string>& servers, std::string& client) {
    if (servers.empty()) {
      return "";
    }
    std::uniform_int_distribution<> dis_(0, servers.size() - 1);
    int index = dis_(gen_);
    return servers[index];
  }

 private:
  std::random_device rd_;
  std::mt19937 gen_;
};
}  // namespace rpc