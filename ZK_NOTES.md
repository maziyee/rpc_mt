# ZooKeeper 服务注册与发现：机制笔记与面试问答

> 关联代码：`rpc_src/common/connect_handler/zk_handler.cpp`、`rpc_src/common/service_registry/service_registry.cpp`
> 关联配置：`config/zk_config.json`、`config/service_config.json`
> 关联文档：`PLAN.md`

## 一、核心机制

### 1.1 先把三个概念分清

| 概念 | 比喻 | 说明 |
|---|---|---|
| **注册** | 把自己的号码写进电话簿 | 服务端启动时做一次 |
| **发现** | 翻电话簿查号码 | 客户端查有哪些实例可用 |
| **watch** | "这页变了叫我一声"的记号 | **不传数据**，只通知"该重查了" |

三者的关系是：**发现靠主动拉取（`zoo_get_children`），watch 只是决定"什么时候该再拉一次"。**

### 1.2 注册流程（服务端）

```
1. 连 ZK，拿到一个 session
2. 确保父路径存在：/rpc_mt、/rpc_mt/<service_name>     ← 持久节点
3. 创建子节点：/rpc_mt/<service_name>/<ip>:<port>      ← 临时节点 ZOO_EPHEMERAL
   data 里存 "<ip>:<port>"
4. 启动完成，开始服务
```

**精髓在"临时节点"**：它和创建它的 session 绑定，所以下面这些情况 ZK 会**自动删除**节点——

- 进程正常退出
- 进程被 `kill -9`
- 机器断电
- 进程死锁卡住、不再发心跳 → session 超时

**服务端不需要写任何"我挂了请帮我摘牌"的代码。** 这是 ZK 做服务发现最核心的价值。

父节点保持持久（`flags = 0`），叶子节点临时（`ZOO_EPHEMERAL`）。进程死了只掉叶子，服务名那一层目录留在 ZK 里。

### 1.3 发现流程（客户端）

```
1. 连 ZK
2. zoo_get_children("/rpc_mt/<service_name>", watch=1, &nodes)
   → 拿到 ["192.168.1.5:8989", "192.168.1.7:8989"]，同时留下 watch
3. 缓存到本地，交给负载均衡挑一台
4. 收到 watch 通知 → 回到第 2 步重新拉
```

### 1.4 watch 到底做了什么

关键区分在这两行：

```c
zoo_get_children(zh, path, 0, &nodes);   // 只拉一次
zoo_get_children(zh, path, 1, &nodes);   // 拉一次 + 在服务端留一个记号
```

**两者的返回值完全一样**，都是一个当前快照。区别只是后者多登记了一条"这里变了通知我"。

```
zoo_get_children(path, watch=1)
      │
      ├──> 立刻返回当前列表 ──────> 客户端缓存，供负载均衡使用
      │
      └──> 同时在 ZK 留一个记号
                    │
              （有实例上线/下线）
                    │
              ZK 推一个通知 ──────> 触发 watcher 回调
                                      ↑ 回调里只有 type/state/path，
                                        没有任何列表数据
                                      │
                                      └──> 再调一次 zoo_get_children 拿新列表
```

**通知里没有数据**，回调签名就说明了这点：

```c
void watcher(zhandle_t* zh, int type, int state, const char* path, void* ctx);
```

只有事件类型、连接状态、路径。想知道变成什么样，必须自己重新拉。

由此推出 watch 的三个反直觉特性：

| 特性 | 原因 |
|---|---|
| 只通知"变了"，不告诉"变成什么" | 它压根不传数据，本质是脏标记 |
| **一次性**——触发后必须重新注册 | 记号用完就没了 |
| **绑在 session 上**——session 没了记号全丢 | 记号登记在连接上 |

> **所以"服务发现是通过 `zoo_get_children` 拿到的"这个理解是对的，加了 watch 之后依然如此。watch 从来不返回数据。**

## 二、会话与故障恢复

### 2.1 必须区分两种情况

这是面试的分水岭。

**情况 A：连接断开（connection loss）—— 不用你操心**

网络抖动、TCP 断开、ZK 集群某个节点重启。ZK 客户端会**自动重连**。

只要在 session timeout 内重连上：

- session 还是原来那个，session id 不变
- **临时节点还在**（ZK 认为你还活着）
- **watch 还在**

所以什么都不用重建，最多打条日志。重连期间发起的请求会返回 `CONNECTIONLOSS`。

**情况 B：会话超时（session expired）—— 灾难，必须重建**

超过 session timeout 都没收到心跳，ZK 判定你死了，然后**不可逆地**：

- 删除你创建的**所有临时节点** ← 服务已从注册表消失
- 丢弃你注册的**所有 watch** ← 从此对变化失聪
- 作废 session id ← 旧 session 永远不能再用

### 2.2 恢复流程

恢复不是"续上"，而是**从头再来一遍**：

```
收到 ZOO_EXPIRED_SESSION_STATE
   │
   ├─ 1. zookeeper_close 关掉旧 handle
   ├─ 2. 重新 zookeeper_init → 全新的 session
   ├─ 3. 等 ZOO_CONNECTED_STATE
   ├─ 4. ★ 重新注册自己的临时节点
   └─ 5. ★ 重新注册所有 watch
```

**第 4、5 步必须都做：**

- 只做 4 不做 5：服务在册了，但列表变了你不知道 → **静默失聪**
- 只做 5 不做 4：你能看到别人，别人看不到你 → **永久下线**

判断走哪条路，靠监听 `ZOO_SESSION_EVENT` 的 state：

| state | 含义 | 动作 |
|---|---|---|
| `ZOO_CONNECTING_STATE` | 正在重连 | 等，别动 |
| `ZOO_CONNECTED_STATE` | 连上了 | 首次连上 → 注册；重连成功 → 确认状态 |
| `ZOO_EXPIRED_SESSION_STATE` | **session 作废** | **重建 handle + 重注册 + 重挂 watch** |
| `ZOO_AUTH_FAILED_STATE` | 认证失败 | 配置问题，重建无用 |

### 2.3 session timeout 怎么定

是个权衡：

- **太大**（如 60s）：进程真挂了，客户端要 60 秒后才发现 → 故障发现慢
- **太小**（如 2s）：网络抖一下就被判超时 → 所有实例同时掉线又同时重注册 → **雪崩**

客户端提议的值会被 ZK 夹到 `[2×tickTime, 20×tickTime]` 区间内。

## 三、面试问答

### Q1：如何实现服务注册与发现？

> 服务端启动时连 ZK，在 `/rpc_mt/<服务名>/` 下创建**临时节点**，节点名和 data 都是 `<ip>:<port>`，父路径是持久节点。用临时节点是因为它和 session 绑定——进程崩溃、被 kill、断电或者卡死导致心跳中断，ZK 都会**自动删除**节点，实现自动摘牌，不需要服务端自己写故障上报。
>
> 客户端启动时用 `zoo_get_children` 拉取该路径下的所有实例，缓存在本地，交给负载均衡挑一台。如果要感知动态上下线，就在 `zoo_get_children` 上挂 watch。

### Q2：会话超时后如何恢复？

> 关键要区分两种失效：**连接断开**和**会话超时**。
>
> 连接断开时 ZK 客户端会自动重连，只要在 session timeout 内连回来，session 不变、临时节点还在、watch 也还在，不需要任何重建。
>
> 但**会话超时**是不可逆的：ZK 会删掉你所有临时节点、丢弃所有 watch、作废 session id。所以恢复必须**重建**——关掉旧 handle、重新 `zookeeper_init` 建新 session、等连上、然后**重新注册临时节点 + 重新挂所有 watch**。这两步缺一不可，少做重注册就永久下线，少做重挂 watch 就静默失聪。

### Q3：watch 是一次性的还是永久的？

> 一次性的。触发一次就失效，想继续监听必须重新注册——这是最常见的 bug 来源。
>
> ZooKeeper 3.6 新增了持久 watch（`addWatch`）和递归 watch，但要用特定 API 和版本。

### Q4：watch 通知里带数据吗？

> 不带。回调只有 `type` / `state` / `path`，本质是个"脏标记"，收到后必须自己重新拉取。
>
> 所以 watch 省的是**空轮询**，不是省掉拉取本身——变更发生后那一次 `zoo_get_children` 是省不掉的。

### Q5：为什么不用轮询？

> 轮询要么间隔短、ZK 压力大，要么间隔长、发现慢，两头不讨好。watch 是变更驱动，只在真变化时推一次通知。
>
> 代价是复杂度：一次性要重注册、绑定 session、session 过期要重挂。**如果客户端数量少、变更不频繁、实时性要求不高，轮询反而是更划算的选择**——代码简单、没有回调线程问题、没有重注册问题。我倾向按场景选，而不是无脑上 watch。

### Q6：watch 的惊群问题？

> 一个节点变化会触发所有 watching 的客户端同时重拉，集群规模大时 ZK 会承受一次集中的读压力。
>
> 缓解方式是客户端侧加随机抖动，不要所有客户端同时重拉。

### Q7：为什么用 ZK 而不是自己写心跳？

> session + 临时节点机制把"故障检测"标准化了：不用自己实现超时判定，也不用处理脑裂。而且 ZK 保证同一个客户端的通知是**有序**的。
>
> 另外 ZK 的 watch 是"本地注册在客户端连接上"的语义——服务端只在变更时往那条连接推一次，不需要维护"谁订阅了什么"的全局表。

## 四、本项目现状对照

### 4.1 已经做对的

- ✅ **注册用了临时节点**（`ZOO_EPHEMERAL`），父路径持久、叶子临时，因此**自动摘牌**这块是正确的
- ✅ 有 `GlobalWatcher` 监听 `ZOO_SESSION_EVENT`，能感知 session 状态变化

### 4.2 缺口

| 环节 | 现状 |
|---|---|
| 发现 | `zoo_get_children` 的 watch 参数是 `0`，**没挂 watch** |
| 拉取时机 | 客户端**只在构造时拉一次**，之后不再刷新 |
| session 超时 | 收到 `ZOO_EXPIRED_SESSION_STATE` 时**只打了行日志**，不重建、不重注册 |
| 重连后 | 不会重新注册临时节点，也不会重挂 watch |

概括：**第一层（会用 API）完整，第二层（懂临时节点模型）做了一半，第三层（session 生命周期管理）完全没有。**

### 4.3 面试时的诚实答法

> 注册用临时节点实现了自动摘牌；发现目前是启动时拉取一次并缓存，没上 watch——因为我这个场景客户端只连一台、生命周期内不重新选路，watch 的价值发挥不出来。
>
> 如果要支持动态摘牌，我会在 `zoo_get_children` 上挂 watch，并且回调里**只置脏标记**、由独立线程重新拉取（回调跑在 ZK 事件线程上，阻塞会卡死整个客户端），同时处理 session 超时后的 handle 重建、重注册和重挂 watch。

**最后这句"我知道该怎么补，只是当前场景不需要"比"我做了 watch"更有说服力**——它说明你在做取舍，而不是照教程抄了一遍。

## 附录：排查中发现的具体问题

供后续动手时参考，与上面的机制说明相互印证。

### ZK 连接层

1. **一个服务端进程开了两条 ZK 连接**
   `ZkHandler::zk_client`（只读，用于发现）和 `ServiceRegistry::zk_handle_`（只写，用于注册）是两条独立的 session。
   后果：**临时节点的生死挂在没人监视的那条连接上**。session B 过期 → 节点消失、服务下线，但 `zoo_state(zk_client)` 仍返回 CONNECTED，进程无感 → **幽灵节点**。
   连带代价：session 数翻倍（`maxClientCnxns` 默认 60/IP，单机实例上限减半）；重连逻辑要在两个类各写一遍。

2. **`ZkHandler::is_running_` 是死变量**
   声明了、初始化成 `false`、`CleanUp()` 里置 `false`，但**从未置 `true`、也从未被读取**。`GlobalWatcher` 里只有日志。

3. **`ZNODEEXISTS` 被当成注册成功**
   `CreateNode` 对 `ZNODEEXISTS` 无条件返回 `true`。快速重启（旧 session 还没超时的 30 秒内）时，新进程会"注册成功"但节点其实属于旧 session；旧 session 过期后节点被删，新进程**永远不再重注册**（注册只在启动时调一次）→ 服务从头到尾不在册。
   修法：用 `Stat.ephemeralOwner` 与 `zoo_client_id()` 比对，判断节点是否真的属于当前 session。

4. **`CleanUp()` 被 `static std::atomic<bool>` 锁成"一辈子只跑一次"**
   导致 session 过期后无法重建，进程永久失效。

5. **`ServiceRegistry::is_connected_` 是裸 `bool`**
   事件线程写、其他线程读，数据竞争（UB）。应改为 `std::atomic<bool>`。

### 服务发现层

6. **客户端只在构造时解析一次服务列表**，且没有 watcher、没有定时刷新
   → 服务上下线对存量客户端不可见；"摘牌"只对新启动的客户端生效。

7. **客户端与 ZK 的连接无法复用于并发请求**
   `RpcClient::Call` 严格串行（发→收→解），`Connect` 只有 `write_mutex_`、读侧无锁；响应**不校验 `sequence_id`**；线上无外层长度前缀，分帧靠"整个接收缓冲是一条消息"。
   → 心跳等并发探测**必须另开连接**。

8. **客户端 `recv_buf_` 永不主动清空**
   `Connect::Read` 是追加，唯一清空它的 `ProgressGetMessage` 只走服务端路径。严格说第二次 `Call` 就会读到上一轮的残留字节。

### 配置层

9. **`service_socket_config.json` 的五个键全部与代码不匹配**
   代码读 `service_ip` / `service_port` / `service_max_connections` / `service_thread_timeout` / `service_name_prefix`，JSON 里是 `servers_ip` / `servers_port` / `max_connections` / `timeout` / `servers_name_prefix`。全部落到硬编码默认值，恰好数值相同所以看不出问题——**改 JSON 无效**。

10. **`zk_config.json` 的 `zk_retry_times` 从未被读取**（代码找的是 `retry_time`），重试间隔永远是默认 3 秒。

11. **`client_config.json` 的 `client_port` 从未被读取**；代码找的 `server_port` 在 JSON 中不存在。

12. **注册地址 ≠ 监听地址**
    监听由 `service_socket_config.json` 决定（且 `Socket::Bind()` 写死 `INADDR_ANY`，`ip_` 被忽略）；注册地址来自 `service_config.json` 的 `registry_nodes`。两者无一致性校验，**跨机部署时客户端会连到它自己**（`127.0.0.1` 是回环）。

13. **`/rpc_mt`（硬编码）与 `/rpc_mt/<service_name>`（配置）靠约定对齐**
    改 `service_name` 会导致注册路径与客户端查询路径不一致，客户端静默拿到空列表。
