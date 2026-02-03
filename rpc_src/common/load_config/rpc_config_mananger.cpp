#include "rpc_config_mananger.h"
#include <fstream>
#include <iostream>
#include "log_manager.h"

bool rpc::RpcConfigManager::Init(const std::string &log_config_path)
{
    try{
        if(!this->spdlog_config_->InitSpdlog(log_config_path)){
            std::cerr << "InitSpdlog config error" << std::endl;
            return false;
        }
        if(!Logger::GetInstance().Init(this->GetSpdlogConfig())){
            std::cerr << "Init Logger error" << std::endl;
            return false;
        }
        LOG_INFO("Init spdlog config success with level: {}" , this->GetSpdlogConfig()->GetLevel());
        return true;
    }catch(const std::exception& e){
        std::cerr <<"Init RpcConfigManager error: " << e.what() << std::endl;
        return false;
    }
}   
