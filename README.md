# rpc_mt

基于 **epoll** 的 C++17 RPC 框架。网络层与报文协议均为自研，
集成 ZooKeeper 服务发现、MySQL 连接池，以及 PBKDF2 密码哈希。

---

## 目录

- [核心能力](#核心能力)
- [架构](#架构)
- [快速开始](#快速开始)
- [测试](#测试)
- [目录结构](#目录结构)
- [已知限制](#已知限制)

---

## 核心能力

### 网络层（自研，不依赖 Asio/muduo）

| 能力 | 实现 |
|---|---|
| IO 模型 | `epoll` **边沿触发（EPOLLET）** + 非阻塞 socket |
| 事件循环 | `ManagerCycle`：单线程 epoll_wait 分发，业务丢线程池 |
| 分帧 | `frame_codec`：明文帧头 `[magic(4)][len(4)]`，处理**半包 / 粘包**，帧长上限 16MB |
| 连接管理 | `ConnectManage`：`fd → Connect` 注册表；`Connect` 持读写缓冲，写路径带互斥锁 |
| 监听 | `Socket`：`SO_REUSEPORT` / `TCP_NODELAY` / `SO_KEEPALIVE` |

### 协议

```
外层  [magic 4B][frame_len 4B][密文]                      ← frame_codec
                    ↓ 解密 / 解压
内层  [RpcHeader 12B][service_name][method_name][payload]  ← rpc_protobuf
```

`RpcHeader` = `magic + body_size + sequence_id`。`payload` 由序列化层决定格式。

### 序列化 / 编解码

- **序列化**：`SerializerManager` 同时支持 **Protobuf** 与 **JSON**，调用方按 `SerializerType` 选
- **压缩**：Zstd
- **报文编码**：见[已知限制](#已知限制) —— 它做的是传输层混淆，不是加密

### 服务治理

- **服务发现**：`ZkHandler` / `ServiceRegistry`，ZooKeeper **临时节点**（进程挂掉自动摘除）
- **负载均衡**：`LoadBalance`，策略可插拔（random / hash / rance）

### 数据与安全

- **`MysqlClient`**：连接池 + 参数化查询（`?` 占位，防注入）
  - 统一的返回契约：`n >= 0` 成功（值为行数），`n < 0` 失败（值为 `-errno`，如 `-1062` = 唯一键冲突）
  - 借还走 **RAII**；坏连接按 `CR_*` 错误码识别并丢弃，池自动补建
  - `available_` 带冷却重试，支持「先起 server 后起 MySQL」自愈
- **`password`**：PBKDF2-HMAC-SHA256（默认 10 万次迭代），自描述编码串
  ```
  pbkdf2_sha256$<iterations>$<salt_hex>$<dk_hex>
  ```
  迭代数**随行存储**，因此调高它不会让存量用户无法登录。

### 业务

- **`AuthService`**：注册 / 登录。密码只以 PBKDF2 编码串落库，任何地方不存明文

### 线程池

`ThreadPool` / `ThreadSingle`：优先级队列（HIGH / NORMAL / LOW）、动态伸缩、
返回 `std::future`。注意 `Enqueue` 在队列满时会**阻塞调用线程**，所以不可从 worker 内再嵌套投递。

---

## 架构

```
                        ┌──────────────┐
      客户端 ─── RPC ───▶│  ManagerCycle │  epoll 事件循环（单线程）
                        └──────┬───────┘
                               │ 收包 → 分帧 → 解密 → 反序列化
                               ▼
                        ┌──────────────┐
                        │ ThreadPool   │  ← 业务在这里跑（阻塞库也只在这里调）
                        └──────┬───────┘
                               ▼
                        ┌──────────────┐
                        │ServiceManager│  service_name → Service 实例
                        └──────┬───────┘
                    ┌──────────┼──────────┐
                    ▼          ▼          ▼
              UserService  RpcService  AuthService
                                          │
                              ┌───────────┴───────────┐
                              ▼                       ▼
                        MysqlClient              password
                        （连接池）              （PBKDF2）
```

**关键约定：所有阻塞调用（MySQL、ZK）都在线程池 worker 里执行，绝不进 epoll 事件循环** ——
否则整个事件循环会被一次慢查询卡住。

---

## 快速开始

### 依赖

以下由 **vcpkg** 提供：

```bash
vcpkg install spdlog nlohmann-json zookeeper zstd protobuf
```

以下用系统包：

```bash
sudo apt install -y libmysqlclient-dev libssl-dev
```

### 构建

```bash
export VCPKG_ROOT=/path/to/vcpkg

cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
```

> ⚠️ **用 `-j4`，不要用裸 `-j`。** GNU make 的 `-j` 是"不限并发"，会一次性 fork
> 上百个 `g++`。`COMMON_SOURCES` 被多个目标各编一遍（约 96 个 TU），
> 在内存有限的机器上会直接触发 swap 风暴。

### 运行

```bash
./scripts/zk_server.sh daemon    # 起本地 ZooKeeper（默认 2181）
./build/server                   # 服务端，0.0.0.0:8989
./build/client                   # 一次 RPC 调用，打印结果
```

配置文件在 `config/`。其中 **`mysql_config.json` 含数据库密码，已加入 `.gitignore`**，
首次使用需按 `mysql_client.h` 里的键名自行创建。

---

## 测试

6 个构建目标，其中 3 个是带 `Check()` 计数的测试（自带 `main`，无 gtest 依赖）：

| 目标 | 需要 | 说明 |
|---|---|---|
| `password_test` | **无** | PBKDF2 的 48 项断言。只链 OpenSSL，**产物 185 KB**（其他目标约 85 MB） |
| `mysql_test` | MySQL | 连接池 12 项：建库建表、`-1062`、`CLIENT_FOUND_ROWS`、并发借还 |
| `auth_test` | MySQL | 注册/登录 32 项，含**直接查库断言存的是哈希而非明文** |
| `framing_test` | ZK + server | 分帧端到端：连续多调用 + 大请求触发半包 |
| `server` / `client` | — | 框架本体 |

```bash
./build/password_test     # 零依赖，随时可跑
./build/mysql_test        # 需先起 MySQL
./build/auth_test

./scripts/zk_server.sh daemon && ./build/server &
./build/framing_test
```

`rpc_src/test/` 下另有若干早期的手工测试（`test_*.cpp`），未进 CMake，
按文件头部注释的 `g++` 命令单独编译。

---

## 目录结构

```
include/                所有对外头文件
protobuf/               生成的 .pb.cc/.h
config/                 各模块 JSON 配置
scripts/                ZooKeeper 启停、连接数测试

rpc_src/
├── client/             RpcClient（调用方）+ client_main
├── server/             server_main + 业务服务（AuthService）
├── rpc_protobuf/       RPC 报文序列化
├── common/
│   ├── network/        epoll 事件循环、连接、连接管理、socket、分帧
│   ├── connect_handler/ ZooKeeper 处理、负载均衡
│   ├── service_registry/ ZK 节点注册
│   ├── thread_pool/    线程池
│   ├── load_config/    各配置类的加载
│   ├── zstd_compress/  压缩
│   ├── aes_encrypt/    报文编码（见「已知限制」）
│   ├── mysql/          MySQL 连接池
│   └── security/       PBKDF2 密码哈希
└── test/               测试
```

---

## 已知限制

写在前面而不是藏着 —— 这些是当前明确**没做**或**做错**的地方。

### 1. 报文编码不是加密

`AesEncrypt` 这个类名沿用自早期版本，但**实现里没有 AES**。它是一个自制流密码：

```cpp
out[i] = (data[i] + key[i % klen] + i) % 256
```

而且主密钥明文写在 `config/thread_pool_aes_config.json` 里、随仓库提交。
**它只提供传输层混淆，不构成任何安全边界。** 需要真正的加密时应换成 OpenSSL EVP。

### 2. 业务错误码到不了客户端

服务的业务错误（如 `user_exists`、`invalid_credentials`）目前**在框架层被丢弃**：

- 服务端：`HandleRequest` 返回 `false` 时，`manager_cycle.cpp` 丢掉出参 `result`，
  改用固定的 `error_code = -1`
- 客户端：`RpcClient::ProcessResponse` 从不读 `error_code`
- 结果：错误响应里 `result_data_` 为空 → 反序列化被跳过 → **`Call` 返回 `true`**，
  调用方分不清"成功但无数据"和"失败"

修法与服务侧约定见仓库内交接文档；在此之前，**不要用 `Call` 的返回值判断业务成败**。

### 3. 鉴权尚不完整

`AuthService` 的注册 / 登录已实现并有 32 项测试，但：

- **登录成功后不签发凭证**（token 未做），所以它目前只解决"注册 + 校验密码"
- `AuthService` **尚未注册进 `server`** —— 代码已编译进二进制，但还没有 `RegisterService`

### 4. 用户名可被枚举

两条通道，均未防护：

- `register` 返回 `user_exists` —— 直接通道，不需要测时间
- `login` 时"用户不存在"立刻返回、而"密码错"要跑约 25ms 的 PBKDF2 —— 时序通道

（`login` 的错误值已统一成 `invalid_credentials`，所以**消息层不泄露**，但耗时差可以统计出来。）

### 5. 尚未实现的功能

好友关系、消息收发、离线消息、群聊、文件传输、在线状态、服务端主动推送、客户端。

---

## 相关文档

仓库内的 `PLAN.md` / `ZK_NOTES.md` / `ARCH_DECOUPLING.md` 是开发过程中的笔记，
按惯例未纳入版本管理，仅在本地保留。
