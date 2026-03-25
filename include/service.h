#pragma once
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

#include "log_manager.h"

namespace rpc {

class Service {
 public:
  Service() = default;
  virtual ~Service() = default;
  virtual std::string GetServiceName() = 0;
  virtual bool HandleRequest(const std::string& method_name,
                             const std::string& args, std::string& result) = 0;
};

class UserService : public Service {
 public:
  UserService() = default;
  ~UserService() override = default;

  std::string GetServiceName() override { return "UserService"; }

  std::string GetUsername(const std::string& user_id) {
    return "user_" + user_id;
  }

  bool HandleRequest(const std::string& method_name, const std::string& args,
                     std::string& result) override {
    if (method_name == "get_username") {
      try {
        auto json_args = nlohmann::json::parse(args);
        nlohmann::json json_result;
        json_result["username"] =
            GetUsername(json_args["user_id"].get<std::string>());
        result = json_result.dump();
        return true;
      } catch (const std::exception& e) {
        LOG_ERROR("UserService HandleRequest failed: {}", e.what());
        result = R"({"error":"invalid args"})";
        return false;
      }
    }
    LOG_ERROR("Unknown method name: {}", method_name);
    return false;
  }
};

class RpcService : public Service {
 public:
  using Handler = std::function<bool(const std::string&, std::string&)>;

  explicit RpcService(std::shared_ptr<UserService> user_svc)
      : user_svc_(std::move(user_svc)) {
    handlers_["echo"] = [this](const std::string& args, std::string& result) {
      return HandleEcho(args, result);
    };
  }
  ~RpcService() override = default;

  std::string GetServiceName() override { return "RpcService"; }

  bool HandleRequest(const std::string& method_name, const std::string& args,
                     std::string& result) override {
    auto it = handlers_.find(method_name);
    if (it == handlers_.end()) {
      LOG_ERROR("Unknown method name: {}", method_name);
      return false;
    }
    LOG_INFO("HandleRequest {}", method_name);
    return it->second(args, result);
  }

 private:
  bool HandleEcho(const std::string& args, std::string& result) {
    try {
      nlohmann::json json_args = nlohmann::json::parse(args);

      std::string echo_str = json_args["message"];
      std::string user_id = json_args.value("user_id", "");

      nlohmann::json json_result;
      json_result["echo"] = "welcome to rpc";
      json_result["receive message"] = echo_str;
      if (!user_id.empty() && user_svc_) {
        json_result["username"] = user_svc_->GetUsername(user_id);
      }
      result = json_result.dump();
      return true;
    } catch (std::exception& e) {
      LOG_ERROR("HandleEcho fail , {}", e.what());
      nlohmann::json json_result;
      json_result["error"] = "invalid args";
      result = json_result.dump();
      return false;
    }
  }

  std::shared_ptr<UserService> user_svc_;
  std::unordered_map<std::string, Handler> handlers_;
};

}  // namespace rpc