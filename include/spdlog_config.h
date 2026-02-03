#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <nlohmann/json.hpp>

namespace rpc{
    class SpdlogConfig{
    public:
    SpdlogConfig() = default;
    ~SpdlogConfig() = default;
    bool InitSpdlog(const std::string& config_path);
    void SetLevel(const std::string&level){level_ = level;};
    const std::string GetLevel()const{return level_;};

    void Setstdout(bool is_stdout){is_stdout_ = is_stdout;};
    bool GetIsStdout()const{return is_stdout_;};

    void SetFileout(bool is_fileout){is_fileout_ = is_fileout;};
    bool GetIsFileout()const{return is_fileout_;};

    void SetLogPath(const std::string& file_path){file_path_ = file_path;};
    std::string GetLogPath()const{return file_path_;};

    SpdlogConfig(const SpdlogConfig&) = delete;
    SpdlogConfig& operator=(const SpdlogConfig&) = delete;
    SpdlogConfig(SpdlogConfig&&) = delete;
    SpdlogConfig& operator=(SpdlogConfig&&) = delete;

    private:
    std::string level_;
    bool is_stdout_;
    bool is_fileout_;
    std::string file_path_;
    };
}


