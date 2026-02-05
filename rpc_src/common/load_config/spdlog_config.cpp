#include "spdlog_config.h"

#include <fstream>
#include <iostream>

bool rpc::SpdlogConfig::InitSpdlog(const std::string& config_path) {
  try {
    std::ifstream spdlog_file(config_path);
    if (!spdlog_file.is_open()) {
      return false;
    }
    nlohmann::json spdlog_config = nlohmann::json::parse(spdlog_file);
    spdlog_file.close();
    this->SetLevel(spdlog_config.value("level", "trace"));
    this->SetFileout(spdlog_config.value("is_fileout", true));
    this->Setstdout(spdlog_config.value("is_stdout", true));
    this->SetLogPath(spdlog_config.value("save_file", "../logs"));
    return true;
  } catch (const std::exception& e) {
    std::cerr << "InitSpdlog config error: " << e.what() << std::endl;
    return false;
  }
}
