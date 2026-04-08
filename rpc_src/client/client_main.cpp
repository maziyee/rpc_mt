#include <filesystem>
#include <nlohmann/json.hpp>

#include "rpc_client.h"

int main() {
  try {
    std::filesystem::path exe_dir =
        std::filesystem::canonical("/proc/self/exe").parent_path();
    std::filesystem::path config_dir = exe_dir / "../config";
    rpc::RpcClient client((config_dir / "client_config.json").string(),
                          (config_dir / "zk_config.json").string());
    std::string args = R"({"message": "Hello, RPC!", "user_id": "42"})";
    nlohmann::json args_json = nlohmann::json::parse(args);
    nlohmann::json res;
    client.Call<nlohmann::json, nlohmann::json>(
        "RpcService", "echo", rpc::SerializerType::JSON, args_json, res);
    std::cout << "RPC success: " << res.dump() << std::endl;
  } catch (const std::exception& e) {
    LOG_ERROR("main error: {}", e.what());
  }
}