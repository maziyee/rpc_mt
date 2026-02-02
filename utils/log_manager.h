#pragma once

#include <string>
#include <vector>
#include <memory>
#include <iostream>
#include <filesystem>

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include"spdlog/sinks/daily_file_sink.h"
#include "fmt/format.h"


#define LOG_TRECE(...) SPDLOG_TRACE(__VA_ARGS__)
#define LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define LOG_INFO(...) SPDLOG_INFO(__VA_ARGS__)
#define LOG_WARN(...) SPDLOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
#define LOG_FATAL(...) SPDLOG_CRITICAL(__VA_ARGS__)


#define CLIENT_INFO(...) SPDLOG_INFO("[client] " __VA_ARGS__)
#define CLIENT_DEBUG(...) SPDLOG_DEBUG("[client] " __VA_ARGS__)
#define CLIENT_WARN(...) SPDLOG_WARN("[client] " __VA_ARGS__)
#define CLIENT_ERROR(...) SPDLOG_ERROR("[client] " __VA_ARGS__)
#define CLIENT_FATAL(...) SPDLOG_CRITICAL("[client] " __VA_ARGS__)


template<>
struct fmt::formatter<std::thread::id> : fmt::formatter<std::string> {
    template <typename FormatContext>
    auto format(std::thread::id &id, FormatContext& ctx) const{
        std::ostringstream oss;
        oss << id;
        return formatter<std::string>::format(oss.str(), ctx);
    }
};


class Logger{
    public:
    static Logger& GetInstance(){
        static Logger instance;
        return instance;
    }

    bool Init(const std::string& log_dir = "../logs"){
        try{
            std::filesystem::path log_path(log_dir);
            if(!std::filesystem::exists(log_path)){
                if(!std::filesystem::create_directories(log_path)){
                    std::cerr <<"fail to create log dir " << std::endl;
                    return false;
                }
            }
            auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            if (console == nullptr){
                std::cerr << "fail to create console sink" << std::endl;
                return false;
            }
            std::string log_file = (log_path / "rpc.log").string();
            auto file_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(log_file, 0, 0);
            if (file_sink == nullptr){
                std::cerr << "fail to create file sink" << std::endl;
                return false;
            }
            std::vector<spdlog::sink_ptr>sinks{console, file_sink};
            this->logger_ = std::make_shared<spdlog::logger>("rpc", sinks.begin(), sinks.end());
            if (logger_ == nullptr){
                std::cerr << "fail to create main logger" << std::endl;
                return false;
            }
            logger_->set_level(spdlog::level::trace);
            logger_->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
            spdlog::set_default_logger(logger_);
            return true;
        } catch(const std::exception& e){
            std::cerr << "fail to init logger " << e.what() << std::endl;
            return false;
        }
    }
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    private:
    Logger() = default;
    ~Logger() = default;
    private:
    std::shared_ptr<spdlog::logger> logger_;
};

