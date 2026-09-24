#pragma once

// MYSQL / MYSQL_STMT 等类型来自官方 C 客户端库，无法前向声明。
#include <mysql/mysql.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "mysql_config.h"

namespace rpc {

// 一行查询结果：各列按 SELECT 顺序排好的字符串
using MysqlRow = std::vector<std::string>;

class MysqlClient;

// ── 两种所有权，两个删除器 ──────────────────────────────────
// 池里的连接有两种生命周期：池自己持有的（空闲时），和借给调用方的。
// 分别用两个删除器表达，让编译器保证两条路径都不会漏。

// 独占所有：析构即关闭。与 zk_handle.h 的删除器同款，先判空再关。
// ⚠️ 写成函数对象而不是函数指针：unique_ptr 的单参构造 unique_ptr(p) 会把删除器
//    【值初始化】—— 换成函数指针就是 nullptr，析构时调用它当场段错误。
//    函数对象没有这个坑，还因为空基类优化不额外占空间。
struct ConnCloser {
  void operator()(MYSQL* conn) const {
    if (conn != nullptr) {
      mysql_close(conn);
    }
  }
};
using OwnedConn = std::unique_ptr<MYSQL, ConnCloser>;

// 借用：析构即归还给池子。broken = true 表示这条连接已经坏了，
// 销毁而不是还回去 —— 还回去的话，下一个借到它的人必然失败，
// 而且症状出现在别人的调用上，极难归因。
struct PoolReturner {
  MysqlClient* client = nullptr;
  bool broken = false;
  void operator()(MYSQL* conn) const;
};
// ⚠️ 这里用 unique_ptr，不像 ZkHandle 那样用 shared_ptr：ZK 会话是【共享】的，
//    而池里的连接是【独占借用】—— shared_ptr 允许拷贝，同一条连接会被归还两次。
// ⚠️ BorrowedConn 不能活得比 MysqlClient 单例久：删除器要回调它。
using BorrowedConn = std::unique_ptr<MYSQL, PoolReturner>;

// 标记「这条连接已损坏」：把 get_deleter() 的写法收在一处，
// 调用点读起来就是一句人话。
inline void MarkBroken(BorrowedConn& conn) {
  if (conn) {
    conn.get_deleter().broken = true;
  }
}

// ⚠️ 所有方法都是【阻塞】的。在 ThreadPool 的 worker 里调，
//    不要在 ManagerCycle 的 epoll 线程里调。
class MysqlClient {
 public:
  static MysqlClient& GetInstance();

  // 建连接池、建库建表。失败不抛异常，只置 available_ = false
  bool Init(const MysqlConfig* config);
  void Close();

  // false = 当前连不上。不是终态，Acquire 会按 kRetryCooldownMs 重试
  bool IsAvailable() const { return this->available_.load(); }

  // ── 返回契约（下面四个执行函数共用）─────────────────────────
  //   n >= 0  成功，值为行数（受影响行数 / 结果集行数）。0 合法，不是失败。
  //   n < 0   失败，值为 -errno（-1062 = ER_DUP_ENTRY）；无连接是 kNoConnection。
  static constexpr int kNoConnection = -1;

  // 无结果集的语句（DDL / INSERT…），走 mysql_real_query
  int Execute(const std::string& sql);

  // 参数化执行，? 占位，返回受影响行数。
  // ⚠️ 返回 0 = "成功但没匹配到任何行"，依赖建连时的 CLIENT_FOUND_ROWS
  int ExecuteParams(const std::string& sql,
                    const std::vector<std::string>& params);

  // 参数化查询，返回结果集行数
  int QueryParams(const std::string& sql,
                  const std::vector<std::string>& params,
                  std::vector<MysqlRow>& rows);

  // 参数化插入，带出 AUTO_INCREMENT 主键；失败时不改 out_insert_id
  int InsertParams(const std::string& sql,
                   const std::vector<std::string>& params,
                   uint64_t& out_insert_id);

  MysqlClient(const MysqlClient&) = delete;
  MysqlClient& operator=(const MysqlClient&) = delete;
  MysqlClient(MysqlClient&&) = delete;
  MysqlClient& operator=(MysqlClient&&) = delete;

 private:
  MysqlClient() = default;
  ~MysqlClient();

  friend struct PoolReturner;  // 它的 operator() 要调下面的 Return

  // 借连接：超时或不可用返回空句柄。拿到即归它管，析构自动归还。
  BorrowedConn Acquire(int timeout_ms);
  // 归还的唯一入口：broken 为真则关掉并腾出名额，否则放回空闲队列。
  // 正常代码里不该直接调 —— 由 PoolReturner 在句柄析构时调。
  void Return(MYSQL* conn, bool broken);

  OwnedConn CreateConnection();    // 连接 + 设超时 + 选库
  bool EnsureDatabase();           // 建库：用不选库的连接
  bool EnsureSchema();             // 建表：前提是库已存在

  static constexpr int kAcquireTimeoutMs = 3000;
  static constexpr int kRetryCooldownMs = 30000;  // 重连冷却

  const MysqlConfig* config_ = nullptr;
  // 空闲连接，池持有。借出时转成 BorrowedConn 交给调用方，
  // 归还时再作为 OwnedConn 收回来。
  std::vector<OwnedConn> idle_;
  size_t created_ = 0;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::atomic<bool> available_{false};
  std::chrono::steady_clock::time_point last_retry_{};
};

}  // namespace rpc
