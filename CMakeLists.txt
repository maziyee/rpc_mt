cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
endif()

project(rpc_mt)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(spdlog CONFIG REQUIRED)

file(GLOB_RECURSE COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/utils/*.cpp")

set (SERVER_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/server/server_main.cpp"
)

add_executable(server
    ${COMMON_SOURCES}
    ${SERVER_SOURCES}
)

foreach(target server)

    target_include_directories(${target} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
    )

    target_link_libraries(${target} PRIVATE
        spdlog::spdlog
    )

    target_compile_options(${target} PRIVATE
        -Wno-deprecated-declarations
        -Wno-unused-parameter
    )

    target_compile_definitions(${target} PRIVATE
        THREADED
        ZK_DEPRECATED=1
    )
endforeach()
