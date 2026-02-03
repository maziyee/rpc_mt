#include "log_manager.h"
#include "rpc_config_mananger.h"
#include <iostream>

int main(){
    if (!rpc::RpcConfigManager::GetInstance().Init("../config/spdlog_config.json")) {
        std::cerr << "RpcConfigManager init error" << std::endl;
        return -1;
    };
    LOG_INFO("RpcConfigManager init success");
    return 0;
}