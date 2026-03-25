#pragma once

#include <future>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "log_manager.h"
#include "service.h"
#include "thread_single.h"

namespace rpc {
class ServiceManager {
 public:
  static ServiceManager& GetInstance() {
    static ServiceManager instance;
    return instance;
  };
  bool RegisterService(std::shared_ptr<Service> service) {
    if (!service) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto service_name = service->GetServiceName();
    if (service_map_.find(service_name) != service_map_.end()) {
      LOG_ERROR("service {} already registered", service_name);
      return false;
    }
    service_map_[service->GetServiceName()] = service;
    LOG_INFO("service {} registered", service_name);
    return true;
  };

  std::shared_ptr<Service> GetService(const std::string& service_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (service_map_.find(service_name) == service_map_.end()) {
      LOG_ERROR("service {} not found", service_name);
      return nullptr;
    }
    return service_map_[service_name];
  };

  bool HandleRequest(const std::string& service_name,
                     const std::string& method_name, const std::string& args,
                     std::string& response) {
    return HandleRequestSync(service_name, method_name, args, response);
  };

  std::future<bool> HandleRequestAsync(const std::string& service_name,
                                       const std::string& method_name,
                                       const std::string& args,
                                       std::shared_ptr<std::string> response) {
    return meeting_ctrl::ThreadSingle::GetInstance().Enqueue(
        meeting_ctrl::TaskPriority::kNORMAL,
        [this, service_name, method_name, args, response]() {
          return HandleRequestSync(service_name, method_name, args, *response);
        });
  };

  meeting_ctrl::ThreadPool::Stats GetThreadStat() const {
    return meeting_ctrl::ThreadSingle::GetInstance().GetStats();
  };

  ServiceManager(const ServiceManager&) = delete;
  ServiceManager& operator=(const ServiceManager&) = delete;

 private:
  ServiceManager() = default;
  ~ServiceManager() = default;
  bool HandleRequestSync(const std::string& service_name,
                         const std::string& method_name,
                         const std::string& args, std::string& response) {
    auto service = GetService(service_name);
    if (!service) {
      LOG_ERROR("service {} not found", service_name);
      return false;
    }
    if (!service->HandleRequest(method_name, args, response)) {
      LOG_ERROR("service {} handle request fail", service_name);
      return false;
    };
    return true;
  };

 private:
  std::unordered_map<std::string, std::shared_ptr<Service>> service_map_;
  std::mutex mutex_;
};
}  // namespace rpc