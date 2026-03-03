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
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-zookeeper CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)

file(GLOB_RECURSE COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/utils/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/common/load_config/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/common/service_registry/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/common/connect_handler/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/common/thread_pool/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/common/zstd_compress/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/common/aes_encrypt/*.cpp"
    )

set (SERVER_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/server/server_main.cpp"
)

set (TEST_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/test/test_aes.cpp"
)

add_executable(server
    ${COMMON_SOURCES}
    ${SERVER_SOURCES}
)

add_executable(test
    ${COMMON_SOURCES}
    ${TEST_SOURCES}
)

foreach(target server test)
    target_include_directories(${target} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(${target} PRIVATE
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        unofficial::zookeeper::zookeeper
        $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static>
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
