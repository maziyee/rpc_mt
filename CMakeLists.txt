cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
endif()

project(rpc_mt)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Debug & AddressSanitizer
# if(NOT CMAKE_BUILD_TYPE)
#     set(CMAKE_BUILD_TYPE Debug)
# endif()
# option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
# if(ENABLE_ASAN)
#     add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
#     add_link_options(-fsanitize=address)
# endif()

find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-zookeeper CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)

# MySQL 客户端库：系统装的 libmysqlclient-dev，用 pkg-config 取库路径。
# ⚠️ 不要用 IMPORTED_TARGET / PkgConfig::MYSQLCLIENT —— 那会把 pkg-config 的
#    编译参数一起传播出去（-isystem /usr/include/mysql），四个目标的编译命令
#    就全变了，已有的 .o 全部作废、整棵构建树重编（这个坑踩过一次）。
#    而头文件其实不需要它：代码写的是 #include <mysql/mysql.h>，/usr/include
#    本来就在默认搜索路径里，直接就能找到。
find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQLCLIENT REQUIRED mysqlclient)

file(GLOB_RECURSE COMMON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/utils/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/common/load_config/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/common/service_registry/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/common/connect_handler/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/common/thread_pool/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/common/zstd_compress/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/common/aes_encrypt/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/protobuf/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/rpc_protobuf/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/common/network/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/common/mysql/*.cpp"
    )

set (SERVER_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/server/server_main.cpp"
)

set (TEST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/test/test_service.cpp"
)

# 客户端核心（不含 main），供 client 与测试目标共用
set(CLIENT_CORE_SOURCES
"${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/client/rpc_client.cpp"
)

set(CLIENT_SOURCES
"${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/client/client_main.cpp"
${CLIENT_CORE_SOURCES}
)

add_executable(server
    ${COMMON_SOURCES}
    ${SERVER_SOURCES}
)

add_executable(client
    ${COMMON_SOURCES}
    ${CLIENT_SOURCES}
)

# 分帧端到端测试（连续多调用 + 大请求触发半包）
# 需要 ZK 与 server 已启动：./scripts/zk_server.sh daemon && ./build/server
add_executable(framing_test
    ${COMMON_SOURCES}
    ${CLIENT_CORE_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/test/test_framing_client.cpp"
)

# MySQL 连接池测试（需要本机 MySQL 已启动，连接信息见 config/mysql_config.json）
add_executable(mysql_test
    ${COMMON_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/test/test_mysql.cpp"
)


foreach(target server client framing_test mysql_test)
    target_include_directories(${target} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/protobuf
    ${Protobuf_INCLUDE_DIRS}
    )
    target_link_libraries(${target} PRIVATE
        protobuf::libprotobuf
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        unofficial::zookeeper::zookeeper
        $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static>
        ${MYSQLCLIENT_LINK_LIBRARIES}
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
