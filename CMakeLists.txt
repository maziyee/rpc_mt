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

# OpenSSL 只用来做 PBKDF2（password_hash）。
# 链接时用 ${OPENSSL_CRYPTO_LIBRARY} 而不是 OpenSSL::Crypto —— 理由同下面的
# MYSQLCLIENT：imported target 会带着 INTERFACE_INCLUDE_DIRECTORIES 把编译
# 参数传播出去，可能让已有的 .o 全部作废、整棵树重编。
find_package(OpenSSL REQUIRED)

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
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/common/security/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/common/push/*.cpp"
    )

# 服务端业务服务（不含 main），供 server 与 auth_test 共用。
# 与 CLIENT_CORE_SOURCES 是同一个模式。
set(SERVER_CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/server/auth_service.cpp"
)

set (SERVER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/server/server_main.cpp"
    ${SERVER_CORE_SOURCES}
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

# AuthService 走【真 RPC】的端到端测试。
# 需要 MySQL + ZK + server 三样都起着 —— 比 framing_test 多一个 MySQL：
#   sudo service mysql start && ./scripts/zk_server.sh daemon && ./build/server
#
# 它同时是「错误链路」的验收：每个业务错误都断言 Call 返回 false 并且
# payload 里能读到 {"error":...}。这两个断言对应修复前的两个缺陷
# （服务端丢 result / 客户端无条件 return true）。
#
# 不需要 SERVER_CORE_SOURCES —— 它是客户端，服务端是另一个进程。
add_executable(auth_rpc_test
    ${COMMON_SOURCES}
    ${CLIENT_CORE_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/test/test_auth_rpc.cpp"
)

# 网络层 5 个修复的测试。
# ①②③⑤ 零外部依赖（自己造回环 TCP）；④ 需要 ZK 起着且 server 未启动。
# 需要 CLIENT_CORE_SOURCES 是因为 ④ 要用 RpcClient。
add_executable(network_test
    ${COMMON_SOURCES}
    ${CLIENT_CORE_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/test/test_network_fixes.cpp"
)

# MySQL 连接池测试（需要本机 MySQL 已启动，连接信息见 config/mysql_config.json）
add_executable(mysql_test
    ${COMMON_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/test/test_mysql.cpp"
)

# AuthService 注册/登录测试（同样需要本机 MySQL）
add_executable(auth_test
    ${COMMON_SOURCES}
    ${SERVER_CORE_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/test/test_auth.cpp"
)

# 协议 type 字段 + Connect 按类型分派。
#
# 刻意【只】列这几个 .cpp，不用 ${COMMON_SOURCES}：Connect 那一层实际只用到
# 协议、分帧、压缩、加密四块，带上 ZK/MySQL/线程池/服务管理是白编一遍。
# 代价是以后 connect.cpp 新增对别的模块的调用时，这里会链接失败 —— 那是
# 好事，说明依赖面变了，应该被看见。
#
# 不需要 ZK 和 server，所以比 framing_test 快也好跑。
add_executable(protocol_test
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/rpc_protobuf/rpc_protobuf.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/common/network/connect.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/common/network/frame_codec.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/common/aes_encrypt/aes_encrypt.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/common/zstd_compress/zstd_compress.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/test/test_protocol.cpp"
)
target_include_directories(protocol_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/protobuf"
    ${Protobuf_INCLUDE_DIRS}
)
target_link_libraries(protocol_test PRIVATE
    protobuf::libprotobuf
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static>
)
target_compile_options(protocol_test PRIVATE
    -Wno-deprecated-declarations
    -Wno-unused-parameter
)

# 密码哈希测试。刻意【不进】下面的 foreach，也不带 ${COMMON_SOURCES} ——
# 它只依赖 OpenSSL，不碰 protobuf/spdlog/zookeeper/mysql。
# 独立配置让它的编译和链接都快得多，也让"这一层零依赖"这件事在构建里可见。
add_executable(password_test
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/test/test_password.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_src/common/security/password_hash.cpp"
)
target_include_directories(password_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(password_test PRIVATE ${OPENSSL_CRYPTO_LIBRARY})


foreach(target server client framing_test mysql_test auth_test network_test
        auth_rpc_test)
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
        # security/password_hash.cpp 在 COMMON_SOURCES 里，所以这四个目标
        # 都编了它、也都得链 libcrypto。漏了就是链接期 undefined reference
        # to EVP_sha256 —— 编译能过，错在最后一步。
        ${OPENSSL_CRYPTO_LIBRARY}
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
