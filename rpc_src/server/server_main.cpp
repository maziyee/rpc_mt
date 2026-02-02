#include "log_manager.h"

#include <iostream>

int main(){
    if (!Logger::GetInstance().Init()){
        std::cerr << "Logger init failed" << std::endl;
        return -1;
    }
    LOG_INFO("Hello World");
    return 0;
}