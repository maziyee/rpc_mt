# RPC 框架更新计划：缓存与持久化（MySQL + Redis）

> 目标对齐简历「高性能分布式 RPC 框架」新增亮点：
> **「集成 Redis 缓存层（hiredis），缓存服务实例列表与热点请求结果，降低 ZooKeeper 拉取与重复计算开销；通过 MySQL 持久化服务注册元数据与调用日志，实现服务治理数据可查询、可追溯。」**

## 现状

- 技术栈：C++17 + vcpkg + CMake；已有依赖 `spdlog / nlohmann-json / zookeeper / zstd / protobuf`
- 已有模块：网络(epoll)、服务注册(ZooKeeper)、负载均衡、线程池、Zstd 压缩、AES 加密、序列化
- 关键落点：
  - `include/zk_handler.h` — `GetServers()` / `UpdateServers()` 拉取服务实例列表 → **Redis 缓存点**
  - `include/service_manager.h` — `HandleRequestSync()` 请求分发 → **调用日志落库点 + 热点请求缓存点**
  - `include/service_registry.h` — `Register()` 服务注册 → **注册元数据落库点**
  - `config/` — JSON 配置（参考 `zk_config.json` 的模式新增 redis/mysql 配置）

## 依赖引入（vcpkg）

```bash
vcpkg install hiredis            # Redis C 客户端
vcpkg install libmysql           # MySQL 官方 C 连接器（或 mariadb-connector-c 二选一）
```

CMakeLists 增加：
```cmake
find_package(hiredis CONFIG REQUIRED)   # 或按 vcpkg 实际导出名调整
find_package(unofficial-libmysql CONFIG REQUIRED)
# target_link_libraries 追加 hiredis / libmysql
```

## 任务拆解

### A. Redis 缓存层

**A1. 封装 `RedisClient`**
- 新增 `include/redis_client.h` + `rpc_src/common/redis/redis_client.cpp`
- 封装 hiredis：连接、`GET/SET/SETEX/DEL`、key 前缀、序列化（用现有 nlohmann/json 把 vector<string> 存成 JSON 字符串）
- 连接失败降级为「无缓存直连」，不阻塞主流程

**A2. 缓存服务实例列表（对齐「降低 ZooKeeper 拉取开销」）**
- 改 `ZkHandler::GetServers()`：先查 Redis key `rpc:servers:<namespace>`，命中直接返回；未命中走 ZooKeeper，取回后写入 Redis 并设 TTL（如 30s）
- `UpdateServers()` / watcher 触发时主动刷新 Redis，保证变更及时

**A3. 缓存热点请求结果（对齐「降低重复计算开销」）**
- 改 `ServiceManager::HandleRequestSync()`：对标记为可缓存的方法（如 `get_username`），先查 Redis key `rpc:cache:<service>:<method>:<args>`，命中直接返回；未命中执行后写回并设 TTL
- 注意：只缓存幂等/结果稳定的方法，写操作不缓存

### B. MySQL 持久化层

**B1. 封装 `MysqlClient`**
- 新增 `include/mysql_client.h` + `rpc_src/common/mysql/mysql_client.cpp`
- 封装连接、`Execute(sql)`、参数化（防注入）、简单连接池/重连
- 建表语句内置（首次连接自动建表）

**B2. 持久化服务注册元数据（对齐「服务治理数据可查询」）**
- 表 `service_registry`：`id / service_name / service_addr / action(register|unregister) / created_at`
- 改 `ServiceRegistry::Register()`（及注销处）：注册成功后异步写一行
- 收益：谁在何时上下线，MySQL 里一查便知

**B3. 持久化调用日志（对齐「调用日志可追溯」）**
- 表 `rpc_call_log`：`id / service_name / method_name / args / response / cost_ms / success / created_at`
- 改 `ServiceManager::HandleRequestSync()`：请求处理完后记录一行（成功/失败、耗时）
- 建议用异步写（线程池/后台队列），不拖慢 RPC 主链路

### C. 配置接入

- 新增 `config/redis_config.json`、`config/mysql_config.json`
- 仿照现有 `RpcConfigManager` / `ZkConfig` 增加 `RedisConfig` / `MysqlConfig` 的加载与读取

### D. 测试与文档

- 单测：`rpc_src/test/` 下加 `test_redis.cpp`、`test_mysql.cpp`（参考现有 test_*.cpp 风格）
- 压测对比：加 Redis 缓存前后 QPS/延迟对比，留一组可写进简历/面试的数据
- 更新 `README.md`（当前还是 GitLab 默认模板，补真实说明）

## 验收标准

每一条都要能对上简历原话、并能在面试讲清楚：

- [ ] Redis 缓存服务实例列表：ZooKeeper 访问次数下降，`GetServers` 命中缓存
- [ ] Redis 缓存热点请求：命中时不再走业务逻辑，延迟下降
- [ ] MySQL 记录服务注册/注销：能查出服务上下线记录
- [ ] MySQL 记录调用日志：能按 service/method 查调用历史与耗时
- [ ] 降级：Redis/MySQL 挂掉时 RPC 主链路仍可用（缓存/日志失效不致命）

## 面试可能追问（照着准备）

- 缓存一致性：服务列表变更时怎么保证 Redis 和 ZK 一致？（答：watcher 主动刷新 + TTL 兜底）
- 缓存穿透/击穿/雪崩：热点请求缓存怎么防？空值缓存？互斥锁？
- 为什么用 MySQL 而不是别的：日志/元数据是关系型、要查询统计，MySQL 合适；量大可再考虑异步批量写入
- 写入会不会拖慢 RPC：异步落库，主链路只写内存/队列

## 建议顺序

`依赖引入 → A1 → B1 → A2 → B2 → A3 → B3 → C → D`
（先跑通 Redis/MySQL 各自封装，再接业务；缓存先做「服务列表」这一最直观的，再做「热点请求」）
