#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <memory>

#include "spdlog_config.h"


namespace rpc{
    class RpcConfigManager{
    public:
        static RpcConfigManager& GetInstance(){
            static RpcConfigManager instance;
            return instance;
        }
        ~RpcConfigManager() = default;
        bool Init(const std::string &log_config_path);

        SpdlogConfig* GetSpdlogConfig() const { return spdlog_config_.get(); }
    private:
        RpcConfigManager(): spdlog_config_(std::make_unique<SpdlogConfig>()){};
    private:
    std::unique_ptr<SpdlogConfig> spdlog_config_;
};
}