#include "load_balance.h"

#include <functional>
#include <unordered_map>

#include "ip_hash.h"
#include "log_manager.h"
#include "rance.h"
#include "random.h"

namespace rpc {
std::mutex LoadBanlance::mut_;
std::shared_ptr<LoadBanlance> LoadBanlance::instance_ = nullptr;
namespace {
struct LoadFactory {
  static std::shared_ptr<LoadBanlance> CreateHash() {
    return std::make_shared<HashLoad>();
  }
  static std::shared_ptr<LoadBanlance> CreateRance() {
    return std::make_shared<RanceLoad>();
  }
  static std::shared_ptr<LoadBanlance> CreateRandom() {
    return std::make_shared<RandomLoad>();
  }
  static const std::unordered_map<
      std::string, std::function<std::shared_ptr<LoadBanlance>()>>&
  GetMap() {
    static const std::unordered_map<
        std::string, std::function<std::shared_ptr<LoadBanlance>()>>
        map = {{"rance", CreateRance},
               {"random", CreateRandom},
               {"hash", CreateHash}};
    return map;
  }
};
}  // namespace

std::shared_ptr<LoadBanlance> LoadBanlance::GetInstance() {
  if (instance_ == nullptr) {
    std::unique_lock<std::mutex> lock(mut_);
    if (!instance_) {
      instance_ = LoadFactory::CreateRandom();
    }
  }
  return instance_;
}

bool LoadBanlance::InitLoadBanlance(const std::string& type) {
  std::lock_guard<std::mutex> lock(mut_);
  auto map = LoadFactory::GetMap();
  auto it = map.find(type);
  if (it != map.end()) {
    try {
      instance_ = it->second();
      LOG_INFO("Init LoadBanlance {}", type);
      return true;
    } catch (std::exception& e) {
      LOG_ERROR("Init failed {}", e.what());
      instance_ = LoadFactory::CreateRandom();
      return false;
    }
  }
  LOG_ERROR("Unkown balance type");
  instance_ = LoadFactory::CreateRandom();
  return false;
}

std::string LoadBanlance::SelectServers(std::vector<std::string>& servers,
                                        std::string& client) {
  if (instance_ == nullptr) {
    LOG_ERROR("the load balance not Init");
    instance_ = LoadFactory::CreateRandom();
  }
  if (servers.empty()) {
    LOG_ERROR("the servers is empty");
    return "";
  }
  if (client.empty()) {
    LOG_ERROR("the client ip is empty");
    return "";
  }
  try {
    auto res = instance_->Select(servers, client);
    return res;
  } catch (const std::exception& e) {
    LOG_ERROR("the select error");
    return servers[0];
  }
}

}  // namespace rpc