# RPC 框架改造计划书（交接版）

> 最后更新：2026-09-17
> 状态：**未开工**（代码中尚无任何 redis / mysql 痕迹）
> 关联文档：[ZK_NOTES.md](ZK_NOTES.md)、[ARCH_DECOUPLING.md](ARCH_DECOUPLING.md)

---

## 一、交接说明

### 为什么有这个改造

简历「高性能分布式 RPC 框架」新增了一条亮点，本文档即为其落地：

> **缓存与持久化**：集成 Redis 缓存层（hiredis），缓存服务实例列表与热点请求结果，降低 ZooKeeper 拉取与重复计算开销；通过 MySQL 持久化服务注册元数据与调用日志，实现服务治理数据可查询、可追溯。

### 分工

| 侧 | 负责人 | 文档 |
| --- | --- | --- |
| **RPC 框架改造** | 接手人（本计划书执行者） | 本文档 |
| **Agent 项目** | Claude | `/home/you_dian/agent/HANDOFF.md` |

两侧互不阻塞，可并行。

### 仓库信息

- 本地路径：`/home/you_dian/rpc/rpc_mt`
- 当前分支：`feature/core`（`main` 已与之同步）
- 远程：`https://github.com/maziyee/rpc_mt.git`（另有 `git.cpptrain.top`）
- ⚠️ 推送 GitHub 需用 `maziyee` 账号且 token 具备 `repo` 权限（曾因权限不足 403）

---

## 二、改造目标

一句话：**给 RPC 框架补上「缓存」与「持久化」两个横切能力。**

拆成四件事：

1. Redis 缓存**服务实例列表** → 减少 ZooKeeper 读压力
2. Redis 缓存**热点请求结果** → 减少重复计算
3. MySQL 记录**服务注册 / 注销元数据** → 服务上下线可追溯
4. MySQL 记录**调用日志** → 调用历史与耗时可查询

---

## 三、现状摸底

### 已有能力

| 模块 | 位置 | 说明 |
| --- | --- | --- |
| 网络（epoll） | `include/socket.h`、`connect.h`、`connect_manage.h`、`manager_cycle.h` | 监听 → 单连接 → 注册表 → 事件循环分发 |
| 服务注册/发现 | `include/zk_handler.h`、`service_registry.h` | ZooKeeper，临时节点 |
| 负载均衡 | `include/load_balance.h` | `LoadFactory` 注册表：hash / random / rance |
| 线程池 | `include/thread_pool.h`、`thread_single.h` | `ThreadSingle` 门面 + 任务优先级 |
| 序列化 | `include/serializer_mananger.h` | Protobuf / JSON |
| 压缩 / 加密 | `include/zstd_compress.h`、`aes_encrypt.h` | Zstd + AES |

### 改造落点（建议，执行前请先核对代码）

| 要做的事 | 落点 | 备注 |
| --- | --- | --- |
| 缓存服务实例列表 | `ZkHandler::GetServers()` / `UpdateServers()` | 拉取前先查 Redis，未命中再走 ZK 并回写 |
| 持久化注册元数据 | `ServiceRegistry::Register()`（及注销处） | 见下方「四、约束 4」，该路径有已知 bug |
| 缓存热点请求结果 | `ServiceManager::HandleRequestSync()` | 只缓存幂等/结果稳定的方法 |
| 持久化调用日志 | `ServiceManager::HandleRequestSync()` | 成功/失败、耗时、service/method |

### 配置文件

`config/` 下现有：`spdlog_config.json`、`thread_pool_aes_config.json`、`service_socket_config.json`、`client_config.json`、`zk_config.json`、`service_config.json`

新增：`redis_config.json`、`mysql_config.json`

---

## 四、关键技术约束（动手前必读）

### 1. 阻塞库不能进 epoll 事件循环 ⚠️ 最重要

hiredis 和 libmysqlclient **都是阻塞库**，而本框架是 epoll 非阻塞 + 线程池模型。

**所有 Redis / MySQL 调用必须丢进已有的线程池执行**：

```cpp
meeting_ctrl::ThreadSingle::GetInstance().Enqueue(
    meeting_ctrl::TaskPriority::kNORMAL,
    [=]() { /* 在这里调 Redis / MySQL */ });
```

直接在主循环里调用会**阻塞整个事件循环**，压测时 QPS 会断崖式下跌。

### 2. 配置键必须与代码读的键**逐字一致** ⚠️

`ZK_NOTES.md` 附录 9~11 已记录：现有三个配置文件的键**全部**与代码不匹配，全部落到硬编码默认值，因数值恰好相同而看不出来——**改 JSON 完全无效**。

新增 `redis_config.json` / `mysql_config.json` 时，务必先看代码里 `GetValue("...")` 读的到底是什么键名，再写 JSON。

### 3. 降级优先，缓存/日志失效不能拖垮主链路

- Redis 连不上 → 直接穿透走 ZooKeeper，不报错、不阻塞
- MySQL 写失败 → 记日志、丢弃该条，不影响 RPC 返回
- 两处都要有超时设置，避免网络异常时线程池被占满

### 4. ZK 注册路径有已知 bug，会影响落库数据的正确性

`ZK_NOTES.md` 附录记录了注册相关的问题，其中这两条直接影响「注册元数据落库」：

- **附录 3**：`ZNODEEXISTS` 被当成注册成功 → 快速重启时会出现「注册成功但实际不在册」，落库会记录假数据
- **附录 4**：`CleanUp()` 被 `static std::atomic` 锁成只跑一次 → session 过期后无法重建

**建议**：先把这两条修掉，再做落库；否则「可追溯」追的是一堆错数据。至少也要在计划里标注这是已知风险。

### 5. ZK 层有两条连接

`ZkHandler::zk_client`（读）和 `ServiceRegistry::zk_handle_`（写）是两条独立 session（附录 1）。做缓存时注意：**服务列表的读取走 `zk_client`**，别混。

---

## 五、任务拆解

### A. Redis 缓存层

**A1. 封装 `RedisClient`**
- 新增 `include/redis_client.h` + `rpc_src/common/redis/redis_client.cpp`
- 封装 hiredis：连接、`GET/SET/SETEX/DEL`、key 前缀、断线重连
- 序列化复用现有 nlohmann/json（`vector<string>` → JSON 字符串）
- **连接失败降级**：标记不可用，后续调用直接跳过缓存

**A2. 缓存服务实例列表**
- 改 `ZkHandler::GetServers()`：先查 `rpc:servers:<namespace>`，命中返回；未命中走 ZK 回写，TTL 建议 30s
- `UpdateServers()` / watcher 触发时主动刷新

**A3. 缓存热点请求结果**
- 改 `ServiceManager::HandleRequestSync()`：对可缓存方法先查 `rpc:cache:<service>:<method>:<args>`
- **只缓存幂等方法**，写操作不缓存
- key 建议带方法版本号，便于失效

### B. MySQL 持久化层

**B1. 封装 `MysqlClient`**
- 新增 `include/mysql_client.h` + `rpc_src/common/mysql/mysql_client.cpp`
- 封装连接、`Execute(sql)`、参数化（防注入）、连接池（取用前 `mysql_ping()` 校验活性）
- 建表语句内置，首次连接自动建表

**B2. 持久化服务注册元数据**

表 `service_registry`：`id / service_name / service_addr / action(register|unregister) / created_at`

**B3. 持久化调用日志**

表 `rpc_call_log`：`id / service_name / method_name / args / response / cost_ms / success / created_at`
- **异步写**（走线程池），不拖慢主链路
- 大字段（args/response）建议截断，避免表膨胀

### C. 配置接入

- 新增 `config/redis_config.json`、`config/mysql_config.json`（**注意约束 2**）
- 仿照现有 `RpcConfigManager` / `ZkConfig` 增加 `RedisConfig` / `MysqlConfig`

### D. 测试与文档

- 单测：`rpc_src/test/` 下加 `test_redis.cpp`、`test_mysql.cpp`（参考现有 `test_*.cpp` 风格）
- CMake：`CMakeLists.txt` 的 `COMMON_SOURCES` 加新目录，`target_link_libraries` 加 hiredis / mysql
- 压测：**加缓存前后对比 QPS/延迟**，留一组可写进简历的数据
- README：当前还是 GitLab 默认模板，补真实说明

---

## 六、验收标准

每一条都要能对上简历原话、并能在面试讲清楚：

- [ ] Redis 缓存服务实例列表：ZK 访问次数下降，`GetServers` 命中缓存
- [ ] Redis 缓存热点请求：命中时不再走业务逻辑，延迟下降
- [ ] MySQL 记录服务注册/注销：能查出服务上下线记录
- [ ] MySQL 记录调用日志：能按 service/method 查调用历史与耗时
- [ ] **降级验证**：手动停掉 Redis / MySQL，RPC 主链路仍正常
- [ ] **压测对比**：加缓存前后 QPS / 平均延迟数据

---

## 七、建议顺序

```
依赖引入(vcpkg) → A1(RedisClient) → B1(MysqlClient)
    → A2(缓存服务列表) → B2(注册元数据落库)
    → A3(缓存热点请求) → B3(调用日志落库)
    → C(配置) → D(测试 + 压测 + README)
```

先跑通各自封装，再接业务；缓存先做「服务列表」（最直观），再做「热点请求」。

---

## 八、依赖与参考项目

### vcpkg 依赖

```bash
vcpkg install hiredis        # Redis C 客户端
vcpkg install libmysql       # 或 mariadb-connector-c，二选一
```

### 参考项目（已克隆到 `/home/you_dian/rpc/references/`）

| 项目 | 看什么 |
| --- | --- |
| `xredis` | **连接池**怎么建、取还连接、断线重连（`src/xRedisClient_connection.cpp`） |
| `redis-plus-plus` | 现代 C++ 封装姿势、连接池配置 |
| `MariaCpp` | MySQL 的 **RAII** 封装（`mariacpp/connection.hpp`、`resultset.hpp`） |

---

## 九、顺手该修的已知问题

来自 `ZK_NOTES.md` 附录，做本次改造时建议一并处理（至少不要踩）：

| # | 问题 | 影响 |
| --- | --- | --- |
| 1 | 一个进程开两条 ZK 连接 | 临时节点生死挂在没人监视的连接上 → 幽灵节点 |
| 3 | `ZNODEEXISTS` 当成成功 | 快速重启后服务永久不在册 |
| 4 | `CleanUp()` 只跑一次 | session 过期后无法重建，进程永久失效 |
| 5 | `is_connected_` 是裸 bool | 数据竞争（UB） |
| 9~11 | 配置文件键全不匹配 | 改 JSON 无效 |
| 12 | 注册地址 ≠ 监听地址 | 跨机部署时客户端连到自己 |

---

## 十、面试准备（改造完成后）

`ZK_NOTES.md` 第三节、`ARCH_DECOUPLING.md` 第六节已备好 Q&A。本次改造新增的可讲点：

- **为什么用 Redis 缓存服务列表**：ZK 读是网络调用，客户端多时是瓶颈；本地缓存 + TTL 兜底
- **缓存一致性**：watcher 主动刷新 + TTL 兜底，双保险
- **为什么异步落库**：调用日志不在关键路径上，同步写会拖慢 RPC
- **降级设计**：缓存和日志都是「有更好，没有也能跑」，不能因为旁路组件挂掉影响主链路
