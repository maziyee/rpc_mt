# 面试素材：开发中的难题 —— 模块解耦与架构优化

> 用途：回答「开发中遇到的最大困难是什么 / 怎么解决的」这一类问题。
> 主线：开发断续 → 模块解耦需求迫切 → 架构上做了哪些解耦 → 对照业界框架进一步吸收。

## 一、面试问题与回答主线（STAR）

### 问题

「开发中遇到的最大问题是什么？你是怎么解决的？」

### 可以直接讲出来的回答

**S — 情境（真实、可信）：**

> 这个 RPC 框架是我断断续续开发的，中间经常隔一两周甚至更久才重新捡起来。我很快发现一个规律：**模块之间耦合越紧，我每次重新上手的成本就越高**——因为要回忆的不只是单个模块本身，还有它和其他模块之间一堆隐式的交互。改动也越危险，改一处经常要连带翻好几处。

**T — 任务（把解耦定性为手段而非目的）：**

> 所以我把「模块解耦」当成了架构上的首要目标。不是为了代码好看，是为了**降低重新理解的成本**：让每个模块能独立读懂、独立修改、独立测试，回归面尽量小。

**A — 行动（对应真实做过的解耦，见第二节）：**

> 我做了几件具体的事：负载均衡策略抽象成接口加工厂、序列化收敛成统一入口、配置按模块拆分、网络层分层、线程池独立成组件。

**R — 结果：**

> 效果是每个模块边界清晰，隔几周回来改负载均衡，不用重新翻网络层和序列化层的代码；新增一种策略或序列化格式，不用碰核心链路。

**深化（拉开差距的关键）：**

> 后来我对照 brpc / gRPC / tRPC 的架构，发现业界框架在解耦上有一个共同的「黄金标准」——**接口抽象 + 注册表 + 拦截器链**。对照下来，我发现自己有三个地方可以吸收，把解耦做得更彻底（见第四节）。

---

## 二、我们已有的解耦成果（真实、带代码位置）

| 模块 | 做法 | 对应机制 | 位置 |
|---|---|---|---|
| 负载均衡 | `LoadBanlance` 纯虚接口 + `LoadFactory::GetMap()` 注册表 | 接口 + 工厂 | [load_balance.h](../rpc_mt/include/load_balance.h)、[load_balance.cpp:15-34](../rpc_mt/rpc_src/common/connect_handler/load_balance.cpp#L15-L34) |
| 序列化 | `SerializerManager` 统一入口，业务只传 `SerializerType` 枚举 | 统一入口（编译期分派） | [serializer_mananger.h](../rpc_mt/include/serializer_mananger.h) |
| 配置 | 每模块一个 Config 类，`RpcConfigManager` 聚合 `unique_ptr` | 模块化配置 | [rpc_config_mananger.h](../rpc_mt/include/rpc_config_mananger.h) |
| 网络 | `Socket`(监听) → `Connect`(单连接) → `ConnectManage`(注册表) → `ManagerCycle`(事件循环+分发) | 分层 | [connect.h](../rpc_mt/include/connect.h)、[manager_cycle.h](../rpc_mt/include/manager_cycle.h) |
| 线程池 | `meeting_ctrl::ThreadPool` + `ThreadSingle` 门面，独立命名空间 | 组件化 | [thread_pool.h](../rpc_mt/include/thread_pool.h)、[thread_single.h](../rpc_mt/include/thread_single.h) |

**值得在面试里强调的一点**：负载均衡这一块，我们已经是**标准的「接口 + 工厂注册表」**模式——`LoadFactory::GetMap()` 里 `unordered_map<string, function<...>>` 注册了 hash/random/rance 三种策略，加新策略就是「实现接口 + 注册一行」，核心链路不用动。**这和 brpc 的 LoadBalancer 注册机制是同一个思路**，是我们解耦做得最完整的地方。

---

## 三、业界框架的解耦机制（对比）

三个框架有一个**共同的黄金标准**，只是实现语言和细节不同：

1. **接口抽象 + 注册表（工厂）**：所有可替换点都定义成接口，通过「名字 → 工厂」注册，运行时按名字选择。
2. **字符串/配置驱动**：不编译期写死，用 `"bns://..."`、`encoding.RegisterCodec(name, ...)` 这种名字选择。
3. **控制面 / 数据面分离**：地址发现、路由决策、治理是后台异步的，和请求处理（数据面）分开。
4. **拦截器 / Filter 链**：横切关注点（日志、监控、鉴权、限流）通过中间件链，不侵入业务。

### brpc（百度，C++，和本项目最可比）

- **命名服务 NamingService** 抽象：`bns` / `file` / `list` 都是它的实现，换注册中心不改业务；采用**控制权反转**——用户拿到节点列表后调 `NamingServiceActions::ResetServers` 通知框架，而不是框架定期调用户。
- **负载均衡 LoadBalancer**：rr / wrr / random / la / c_md5 等，字符串指代 + 全局注册。
- **协议 Protocol**：同一个端口靠**协议探测**支持 baidu_std / HTTP / gRPC / Redis 等多协议。

### gRPC

- **Resolver 与 Balancer 分离**：各自是独立模块，通过 `Register(Builder)` 全局注册表（Builder 就是工厂），靠唯一的 `Scheme()` 名字注册；两边用独立的回调接口，体现「限界上下文」。
- **Codec 接口**：`Marshal / Unmarshal / Name` + `encoding.RegisterCodec()`，可替换为 JSON 等自定义编解码。
- **Interceptor**：客户端/服务端中间件链，任意数量、层层嵌套。

### tRPC（腾讯，多语言）

- **插件工厂 `plugin.Factory`**：`Type() + Setup(name, Decoder)`，两级管理（类型 → 名称），调用 `plugin.Register` 注册。
- **依赖管理**：`Depender`（强依赖）/ `FlexDepender`（弱依赖），插件按依赖顺序初始化。
- **插件分类**：Codec（协议+序列化+压缩）、Naming（注册+发现+负载均衡+熔断）、Config、Metrics、Logging、Tracing、Filter 七类，全部是插件。

---

## 四、吸收方向：把框架更好的做法变成我们的

诚实地说，我们做的是「雏形」，框架做的是「彻底」。下面三个吸收方向，既能补上差距，又能在面试里展示「我知道差距在哪、怎么演进」。

### 1. 序列化：从「枚举 + if constexpr」升级为「接口 + 注册表」

**现状**（[serializer_mananger.h](../rpc_mt/include/serializer_mananger.h)）：用 `enum SerializerType` + `switch` + `if constexpr` 做编译期分派。

**问题**：加一种序列化（比如 msgpack），要改 enum、改 switch、改 `SerializerManager` 本体——**扩展要动框架代码**。

**吸收**（对齐 gRPC 的 `encoding.RegisterCodec`）：定义 `Serializer` 接口，用名字注册：

```cpp
struct Serializer {
  virtual std::string Serialize(const google::protobuf::Message&) = 0;
  virtual bool Deserialize(const std::string&, google::protobuf::Message&) = 0;
  virtual std::string Name() = 0;
};
// 注册表
void RegisterSerializer(std::unique_ptr<Serializer> s);
```

加新序列化 = 实现接口 + 注册，**框架零改动**。这正好也解决了一个真实痛点：现在 `JsonSerializer::Serialize<T>` 只在 `T == nlohmann::json` 时能实例化，换接口后这个坑一并填上。

### 2. 服务发现：硬编码 ZK → 抽象 NamingService 接口

**现状**：服务发现/注册硬编码在 `ZkHandler` + `ServiceRegistry`，还因此开了两条连接（一条读一条写），有状态盲区（见 `ZK_NOTES.md` 附录第 1 条）。

**问题**：换 Consul / etcd 要重写；注册和发现没抽象成一个统一职责，导致临时节点挂在没人监视的连接上。

**吸收**（对齐 brpc 的 NamingService）：抽两个职责接口：

```cpp
struct Registry {       // 服务注册（写）
  virtual bool Register(const std::string& service, const std::string& addr) = 0;
  virtual bool Deregister(const std::string& service, const std::string& addr) = 0;
};
struct Discovery {      // 服务发现（读）
  virtual std::vector<std::string> GetServers(const std::string& service) = 0;
};
// ZK 只是其中一个实现
```

**这个吸收顺带解决一个真实 bug**：把注册和发现统一到同一个接口族，就自然会共用一个连接（一个 session），之前「两条连接、幽灵节点」的问题从根上消失。

### 3. 编解码链：硬编码顺序 → Codec 链

**现状**（[connect.cpp](../rpc_mt/rpc_src/common/network/connect.cpp) 的 `Write`）：`序列化 → Zstd 压缩 → AES 加密` 顺序写死在 `Connect::Write` 里。

**问题**：加一个步骤（比如加一层校验、换一种压缩）要改 `Connect` 本体；`Connect` 既管网络又管编解码，职责过重。

**吸收**（对齐 tRPC 的 Codec 插件）：把「序列化、压缩、加密」抽象成可装配的 Codec 链，`Connect` 只负责「把字节发出去」。

### 4.（可选项）加 Filter 链

**现状**：横切逻辑（日志、统计）散落在各处，没有统一插入点。

**吸收**（对齐 gRPC Interceptor / tRPC Filter）：在 `HandleMessageSync` 前后插入中间件链，日志、监控、鉴权、限流都能在不改业务的情况下装配。

---

## 五、面试时可能的追问与应对

**Q：你这不是把 brpc 抄过来了吗？**
> 答：不是抄，是「用业界验证过的模式对照自己的实现」。我承认我们做的是雏形，覆盖不全——但正因为断续开发，我更在意「方向对不对」。对照框架之后，我确认了「接口 + 注册表」这个方向是对的，只是还有三个点没做彻底。这比闷头重造轮子更靠谱。

**Q：解耦会不会过度设计？单机 RPC 有必要吗？**
> 答：解耦的动机不是「未来可能用到」，而是「断续开发时降低重上手成本」。如果一个模块要联动回忆三个其他模块，我每次捡起来都要多花时间。所以对我来说，解耦是**当下**的收益，不是未来的。当然我也会克制——比如方法级热替换我就没做，因为现在没有那个需求。

**Q：为什么不直接用现成框架？**
> 答：目标是搞懂 RPC 框架的每个细节，自己写是最快的学习路径。而解耦是这份代码「可维护性」的体现，和「能跑」是两个维度。

---

## 六、一页纸速记（面试前扫一眼）

- **主线**：断续开发 → 耦合越紧重上手越贵 → 解耦成为架构目标
- **已做**：负载均衡（接口+工厂）、序列化（统一入口）、配置（分模块）、网络（分层）、线程池（组件化）
- **最完整的是负载均衡**，已经是标准接口+注册表
- **对照框架发现的三个吸收点**：序列化注册表化、服务发现抽象 NamingService、编解码 Codec 链
- **诚实立场**：我们是雏形，框架做得更彻底，但我知道差距在哪、怎么演进
