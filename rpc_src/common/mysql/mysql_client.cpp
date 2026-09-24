#include "mysql_client.h"

#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "log_manager.h"

namespace rpc {
namespace {

// 把 MySQL 的错误码折成本类的返回契约：失败返回 -errno。
// errno 为 0 时【不能】返回 -0 —— 那等于 0，调用方会读成"成功、影响 0 行"。
int FailCode(unsigned int err) {
  return err != 0 ? -static_cast<int>(err) : MysqlClient::kNoConnection;
}

// 行数 → int，超上限截到 INT_MAX
int ClampInt(uint64_t v) {
  constexpr uint64_t kMax =
      static_cast<uint64_t>(std::numeric_limits<int>::max());
  return v > kMax ? std::numeric_limits<int>::max() : static_cast<int>(v);
}

// MySQL 客户端错误码（CR_*）从 2000 起，服务端错误（ER_*）从 1000 起。
// CR_* 说明这条连接本身出问题了，不能再放回池里 —— 否则下一个借到它的人
// 必然失败，而且症状出现在别人的调用上，极难归因。
bool ConnBroken(unsigned int err) { return err >= 2000; }

struct PreparedResult {
  int code = MysqlClient::kNoConnection;  // >=0 行数，<0 失败
  uint64_t insert_id = 0;
  bool conn_broken = false;
};

// 在一条【已经借到】的连接上跑参数化语句，结果集读进 *rows（nullptr
// 表示不取）。 本函数不归还连接 —— 由调用方根据 conn_broken 决定 Release 还是
// Drop。
PreparedResult RunPrepared(MYSQL* conn, const std::string& sql,
                           const std::vector<std::string>& params,
                           std::vector<MysqlRow>* rows) {
  PreparedResult out;

  MYSQL_STMT* stmt = mysql_stmt_init(conn);
  if (stmt == nullptr) {
    LOG_ERROR("mysql_client: stmt_init failed: {}", mysql_error(conn));
    out.code = FailCode(mysql_errno(conn));
    out.conn_broken = true;
    return out;
  }
  // stmt 持有结果集，每条退出路径都要关掉它
  struct StmtGuard {
    MYSQL_STMT* stmt;
    ~StmtGuard() { mysql_stmt_close(stmt); }
  } stmt_guard{stmt};

  if (mysql_stmt_prepare(stmt, sql.c_str(), sql.size()) != 0) {
    const unsigned int err = mysql_stmt_errno(stmt);
    LOG_ERROR("mysql_client: prepare failed: {} (errno={})",
              mysql_stmt_error(stmt), err);
    out.code = FailCode(err);
    out.conn_broken = ConnBroken(err);
    return out;
  }

  // 参数个数对不上是调用方的 bug，在这里失败比绑越界强
  const unsigned long want = mysql_stmt_param_count(stmt);
  if (want != params.size()) {
    LOG_ERROR("mysql_client: param count mismatch: sql needs {}, got {}", want,
              params.size());
    out.code = -EINVAL;  // 没有 MySQL 错误码可用；真实 errno 都 >= 1000，不会撞
    return out;
  }

  // binds / lengths / is_null 必须活到 mysql_stmt_execute 之后
  const size_t n = params.size();
  std::vector<MYSQL_BIND> binds(n);
  std::vector<unsigned long> lengths(n);
  // 不能是 std::vector<bool>：它是位域特化，&v[i] 取不到 bool*
  std::unique_ptr<bool[]> is_null(new bool[n]());
  for (size_t i = 0; i < n; ++i) {
    lengths[i] = static_cast<unsigned long>(params[i].size());
    binds[i].buffer_type = MYSQL_TYPE_STRING;
    binds[i].buffer = const_cast<char*>(params[i].data());  // C API 只读
    binds[i].buffer_length = lengths[i];
    binds[i].length = &lengths[i];
    binds[i].is_null = &is_null[i];
  }
  if (n > 0 && mysql_stmt_bind_param(stmt, binds.data()) != 0) {
    const unsigned int err = mysql_stmt_errno(stmt);
    LOG_ERROR("mysql_client: bind_param failed: {} (errno={})",
              mysql_stmt_error(stmt), err);
    out.code = FailCode(err);
    out.conn_broken = ConnBroken(err);
    return out;
  }

  if (mysql_stmt_execute(stmt) != 0) {
    const unsigned int err = mysql_stmt_errno(stmt);
    LOG_ERROR("mysql_client: execute failed: {} (errno={})",
              mysql_stmt_error(stmt), err);
    out.code = FailCode(err);
    out.conn_broken = ConnBroken(err);
    return out;
  }

  out.insert_id = mysql_stmt_insert_id(stmt);

  if (rows == nullptr) {
    out.code = ClampInt(mysql_stmt_affected_rows(stmt));
    return out;
  }

  MYSQL_RES* meta = mysql_stmt_result_metadata(stmt);
  if (meta == nullptr) {
    // 语句不产生结果集（比如把 UPDATE 传给了 QueryParams）—— 0 行，不算失败
    rows->clear();
    out.code = 0;
    return out;
  }
  struct MetaGuard {
    MYSQL_RES* res;
    ~MetaGuard() { mysql_free_result(res); }
  } meta_guard{meta};

  if (mysql_stmt_store_result(stmt) != 0) {
    const unsigned int err = mysql_stmt_errno(stmt);
    LOG_ERROR("mysql_client: store_result failed: {} (errno={})",
              mysql_stmt_error(stmt), err);
    out.code = FailCode(err);
    out.conn_broken = ConnBroken(err);
    return out;
  }

  const unsigned int field_count = mysql_num_fields(meta);
  MYSQL_FIELD* fields = mysql_fetch_fields(meta);
  std::vector<std::vector<char>> buffers(field_count);
  std::vector<unsigned long> out_lengths(field_count);
  std::unique_ptr<bool[]> out_is_null(new bool[field_count]());
  std::unique_ptr<bool[]> out_errors(new bool[field_count]());
  std::vector<MYSQL_BIND> out_binds(field_count);
  for (unsigned int i = 0; i < field_count; ++i) {
    // 用列的【声明宽度】(MYSQL_FIELD::length)，不用 max_length —— 后者默认
    // 不计算，要靠 STMT_ATTR_UPDATE_MAX_LENGTH 显式打开；不开就恒为 0，
    // 按它分配会得到 1 字节缓冲、把每个值都截成空串，静默丢数据。
    // 声明宽度一定不小于实际值，是安全的上界。
    buffers[i].resize(static_cast<size_t>(fields[i].length) + 1);
    out_binds[i].buffer_type = MYSQL_TYPE_STRING;
    out_binds[i].buffer = buffers[i].data();
    out_binds[i].buffer_length = static_cast<unsigned long>(buffers[i].size());
    out_binds[i].length = &out_lengths[i];
    out_binds[i].is_null = &out_is_null[i];
    out_binds[i].error = &out_errors[i];
  }
  if (field_count > 0 && mysql_stmt_bind_result(stmt, out_binds.data()) != 0) {
    const unsigned int err = mysql_stmt_errno(stmt);
    LOG_ERROR("mysql_client: bind_result failed: {} (errno={})",
              mysql_stmt_error(stmt), err);
    out.code = FailCode(err);
    out.conn_broken = ConnBroken(err);
    return out;
  }

  rows->clear();
  for (;;) {
    const int rc = mysql_stmt_fetch(stmt);
    if (rc == MYSQL_NO_DATA) {
      break;
    }
    if (rc == 1) {  // 0=成功 1=出错 100=无数据 101=被截断
      const unsigned int err = mysql_stmt_errno(stmt);
      LOG_ERROR("mysql_client: fetch failed: {} (errno={})",
                mysql_stmt_error(stmt), err);
      out.code = FailCode(err);
      out.conn_broken = ConnBroken(err);
      return out;
    }
    MysqlRow row;
    row.reserve(field_count);
    for (unsigned int i = 0; i < field_count; ++i) {
      if (out_is_null[i]) {
        row.emplace_back();  // NULL 和空串在 MysqlRow 里都是空串
      } else {
        // 被截断时 out_lengths[i] 是【原值长度】，比缓冲还大，直接用会读越界
        size_t len = out_lengths[i];
        if (len > buffers[i].size()) {
          len = buffers[i].size();
        }
        row.emplace_back(buffers[i].data(), len);
      }
    }
    rows->push_back(std::move(row));
  }
  out.code = ClampInt(rows->size());
  return out;
}

// 建一条裸连接：init + 三个超时 + connect。
// db 传 nullptr 表示不选库 —— 建库时必须这样，因为库此刻还不存在。
// 返回 OwnedConn：init 之后就拿到的句柄立即归它管，任何一条失败路径
// 都不必再手写 mysql_close。
OwnedConn ConnectRaw(const MysqlConfig& cfg, const char* db,
                     unsigned long flags) {
  OwnedConn conn(mysql_init(nullptr));
  if (!conn) {
    LOG_ERROR("mysql_client: mysql_init failed (out of memory)");
    return nullptr;
  }

  // 三个超时都要设。只设连接超时的话，连上之后若 MySQL 挂起不响应，
  // 调用会永久阻塞在 recv 上，worker 被吃干净。
  const unsigned int timeout =
      static_cast<unsigned int>(cfg.GetConnectTimeoutSec());
  mysql_options(conn.get(), MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
  mysql_options(conn.get(), MYSQL_OPT_READ_TIMEOUT, &timeout);
  mysql_options(conn.get(), MYSQL_OPT_WRITE_TIMEOUT, &timeout);

  // caching_sha2_password 在【明文 TCP】上做完整认证时，要先用服务器的 RSA
  // 公钥加密密码。libmysqlclient 默认不主动索要那把公钥，于是第一次连接被拒。
  // 打开这个开关才能通 —— 之后服务器缓存了密码摘要，走快速认证就不需要了。
  // （空密码不走这条路，所以空密码配置下看不出问题。）
  bool get_server_public_key = true;
  mysql_options(conn.get(), MYSQL_OPT_GET_SERVER_PUBLIC_KEY,
                &get_server_public_key);

  if (mysql_real_connect(conn.get(), cfg.GetHost().c_str(),
                         cfg.GetUser().c_str(), cfg.GetPassword().c_str(), db,
                         static_cast<unsigned int>(cfg.GetPort()), nullptr,
                         flags) == nullptr) {
    LOG_ERROR("mysql_client: connect failed: {}", mysql_error(conn.get()));
    return nullptr;  // 句柄析构 → mysql_close
  }
  return conn;
}

// 建表语句。库由连接选定，这里不写库名。
const char* const kSchemaSql[] = {
    R"(CREATE TABLE IF NOT EXISTS users (
         uid           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
         username      VARCHAR(64)     NOT NULL,
         password_hash VARCHAR(255)    NOT NULL,
         created_at    TIMESTAMP       NOT NULL DEFAULT CURRENT_TIMESTAMP,
         updated_at    TIMESTAMP       NOT NULL DEFAULT CURRENT_TIMESTAMP
                                       ON UPDATE CURRENT_TIMESTAMP,
         PRIMARY KEY (uid),
         UNIQUE KEY uk_users_username (username)
       ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4)",
    R"(CREATE TABLE IF NOT EXISTS messages (
         msg_id     BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
         from_uid   BIGINT UNSIGNED NOT NULL,
         to_uid     BIGINT UNSIGNED NOT NULL,
         content    VARCHAR(4096)   NOT NULL,
         created_at TIMESTAMP(3)    NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
         PRIMARY KEY (msg_id),
         KEY idx_msg_to_uid   (to_uid, msg_id),
         KEY idx_msg_from_uid (from_uid, msg_id)
       ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4)",
};

}  // namespace

MysqlClient& MysqlClient::GetInstance() {
  static MysqlClient instance;
  return instance;
}

MysqlClient::~MysqlClient() { this->Close(); }

// 借用句柄的删除器：句柄析构时把连接交还给池子（或按 broken 销毁）。
// 它要回调私有的 Return，所以 PoolReturner 在 MysqlClient 里声明成了 friend。
void PoolReturner::operator()(MYSQL* conn) const {
  if (conn == nullptr) {
    return;  // unique_ptr 不会拿空指针调删除器，这里只是兜底
  }
  this->client->Return(conn, this->broken);
}

void MysqlClient::Close() {
  // 借出去的连接这里够不到，由 Return() 在发现 available_ 为 false 时关掉。
  //
  // ⚠️ dead
  // 必须声明在锁作用域【外】：声明在作用域内的话，析构顺序是
  //    「后声明的先销毁」，删除器（= mysql_close，有 I/O）会在持锁状态下跑。
  std::vector<OwnedConn> dead;
  {
    std::lock_guard<std::mutex> lock(this->mutex_);
    dead.swap(this->idle_);
  }
  dead.clear();  // 关连接在锁外
  this->available_.store(false);
}

OwnedConn MysqlClient::CreateConnection() {
  // CLIENT_FOUND_ROWS：让 UPDATE 返回"被 WHERE 匹配到的行数"，而不是"真正被
  // 改动的行数"。不带的话，把字段更新成它原本的值会返回
  // 0，被误判成"记录不存在"。
  return ConnectRaw(*this->config_, this->config_->GetDatabase().c_str(),
                    CLIENT_FOUND_ROWS);
}

bool MysqlClient::EnsureDatabase() {
  // 不选库：库还不存在，带了 db 名 mysql_real_connect 会直接失败。
  // 这就是建库和建表必须分成两步的原因。
  OwnedConn conn = ConnectRaw(*this->config_, nullptr, 0);
  if (!conn) {
    return false;
  }
  // 库名来自配置、不是用户输入；用反引号包住以免有特殊字符。
  const std::string sql = "CREATE DATABASE IF NOT EXISTS `" +
                          this->config_->GetDatabase() +
                          "` DEFAULT CHARACTER SET utf8mb4";
  const bool ok = mysql_real_query(conn.get(), sql.c_str(), sql.size()) == 0;
  if (!ok) {
    LOG_ERROR("mysql_client: create database failed: {}",
              mysql_error(conn.get()));
  }
  return ok;  // 句柄析构 → mysql_close
}

bool MysqlClient::EnsureSchema() {
  OwnedConn conn = this->CreateConnection();  // 此时库已建好
  if (!conn) {
    return false;
  }
  bool ok = true;
  for (const char* sql : kSchemaSql) {
    if (mysql_real_query(conn.get(), sql, std::strlen(sql)) != 0) {
      LOG_ERROR("mysql_client: create table failed: {}",
                mysql_error(conn.get()));
      ok = false;
      break;
    }
  }
  return ok;
}

bool MysqlClient::Init(const MysqlConfig* config) {
  if (config == nullptr) {
    LOG_ERROR("mysql_client: null config");
    return false;
  }
  this->config_ = config;

  // 重试路径也会走到这，先清掉上一次可能残留的连接。
  this->Close();

  if (!this->EnsureDatabase() || !this->EnsureSchema()) {
    LOG_ERROR("mysql_client: schema not ready");
    return false;
  }

  const int pool_size = config->GetPoolSize();
  std::vector<OwnedConn> conns;
  conns.reserve(static_cast<size_t>(pool_size));
  for (int i = 0; i < pool_size; ++i) {
    OwnedConn conn = this->CreateConnection();
    if (!conn) {
      break;  // 部分成功也够用，只要能借到就行
    }
    conns.push_back(std::move(conn));
  }
  if (conns.empty()) {
    LOG_ERROR("mysql_client: no connection could be created");
    return false;
  }
  if (static_cast<int>(conns.size()) < pool_size) {
    LOG_WARN("mysql_client: pool only {}/{} connections", conns.size(),
             pool_size);
  }

  {
    std::lock_guard<std::mutex> lock(this->mutex_);
    for (OwnedConn& conn : conns) {
      this->idle_.push_back(std::move(conn));
    }
    this->created_ = conns.size();
  }
  this->available_.store(true);
  LOG_INFO("mysql_client: ready, pool={}, {}", conns.size(),
           config->Describe());
  return true;
}

BorrowedConn MysqlClient::Acquire(int timeout_ms) {
  if (this->config_ == nullptr) {
    return nullptr;  // Init 从未被调用过
  }
  if (!this->available_.load()) {
    // 不可用时按冷却重试 Init，而不是永远失败 —— 这样"先起 server 后起
    // MySQL"能自愈。判断和写时间戳在同一把锁里，保证同一时刻只有一个线程
    // 走进 Init（Init 会重建池，并发进去会互相踩）。
    const auto now = std::chrono::steady_clock::now();
    bool retry = false;
    {
      std::lock_guard<std::mutex> lock(this->mutex_);
      if (now - this->last_retry_ >=
          std::chrono::milliseconds(kRetryCooldownMs)) {
        this->last_retry_ = now;
        retry = true;
      }
    }
    if (!retry || !this->Init(this->config_)) {
      return nullptr;
    }
  }

  const auto pool_size = static_cast<size_t>(this->config_->GetPoolSize());
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  std::unique_lock<std::mutex> lock(this->mutex_);
  bool tried_create = false;
  for (;;) {
    if (!this->idle_.empty()) {
      OwnedConn owned = std::move(this->idle_.back());
      this->idle_.pop_back();
      // 池持有 → 借出：所有权策略在这里切换。release() 交出的裸指针立刻被
      // 新句柄接管，中间没有无主的瞬间；之后句柄析构时自动归还。
      return BorrowedConn(owned.release(), PoolReturner{this});
    }
    if (!this->available_.load()) {
      return nullptr;  // 等待期间被 Close 了
    }
    // 池空且没到上限：补一条。坏连接在 Return 里被丢掉，靠这条自愈；
    // 不做的话池会越用越少，最后所有请求都卡在超时上。
    if (!tried_create && this->created_ < pool_size) {
      tried_create = true;
      ++this->created_;  // 先占位，避免并发下超建
      lock.unlock();
      OwnedConn owned = this->CreateConnection();
      lock.lock();
      if (owned) {
        return BorrowedConn(owned.release(), PoolReturner{this});
      }
      --this->created_;  // 没建成，还回占位
    }
    if (!this->cv_.wait_until(lock, deadline,
                              [this] { return !this->idle_.empty(); })) {
      LOG_WARN("mysql_client: acquire timeout {}ms (idle={} created={})",
               timeout_ms, this->idle_.size(), this->created_);
      return nullptr;
    }
  }
}

// 归还的唯一入口，由 PoolReturner 在借用句柄析构时调用。
// 原来的两条路径（Release 归还 / DropConnection 丢弃）本来就只有一处区别 ——
// 要不要放回空闲队列 —— 这里按 broken 和池的存活状态一次判完，记账逻辑不变。
void MysqlClient::Return(MYSQL* conn, bool broken) {
  if (conn == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(this->mutex_);
    // 连接没坏 + 池还活着 → 放回空闲队列，等下一个借的人
    if (!broken && this->available_.load()) {
      this->idle_.push_back(OwnedConn(conn));
      this->cv_.notify_one();
      return;
    }
    // 否则（连接坏了，或是 Close() 之后才归还的）：腾出名额，Acquire
    // 会补一条新的
    if (this->created_ > 0) {
      --this->created_;
    }
  }
  mysql_close(conn);  // 关连接有 I/O，放到锁外
}

int MysqlClient::Execute(const std::string& sql) {
  BorrowedConn handle = this->Acquire(kAcquireTimeoutMs);
  if (!handle) {
    return kNoConnection;
  }
  MYSQL* const conn = handle.get();  // 句柄管所有权，裸指针只用来调 C API

  int code = kNoConnection;
  bool broken = false;
  if (mysql_real_query(conn, sql.c_str(), sql.size()) != 0) {
    const unsigned int err = mysql_errno(conn);
    LOG_ERROR("mysql_client: Execute failed: {} (errno={})", mysql_error(conn),
              err);
    code = FailCode(err);
    broken = ConnBroken(err);
  } else if (mysql_field_count(conn) == 0) {
    code = ClampInt(mysql_affected_rows(conn));
  } else {
    // 语句意外带了结果集（比如把 SELECT 传给了 Execute）。必须收干净，
    // 否则连接上留着没读完的结果，这条连接下次被谁借到，谁就报
    // "Commands out of sync" —— 症状出现在别人的调用上。
    MYSQL_RES* res = mysql_store_result(conn);
    if (res == nullptr) {
      const unsigned int err = mysql_errno(conn);
      LOG_ERROR("mysql_client: store_result failed: {} (errno={})",
                mysql_error(conn), err);
      code = FailCode(err);
      broken = ConnBroken(err);
    } else {
      code = ClampInt(mysql_num_rows(res));
      mysql_free_result(res);
    }
  }

  if (broken) {
    MarkBroken(handle);
  }
  return code;  // 出作用域，句柄析构 → 自动归还
}

int MysqlClient::ExecuteParams(const std::string& sql,
                               const std::vector<std::string>& params) {
  BorrowedConn handle = this->Acquire(kAcquireTimeoutMs);
  if (!handle) {
    return kNoConnection;
  }
  MYSQL* const conn = handle.get();
  const PreparedResult r = RunPrepared(conn, sql, params, nullptr);
  if (r.conn_broken) {
    MarkBroken(handle);
  }
  return r.code;
}

int MysqlClient::QueryParams(const std::string& sql,
                             const std::vector<std::string>& params,
                             std::vector<MysqlRow>& rows) {
  BorrowedConn handle = this->Acquire(kAcquireTimeoutMs);
  if (!handle) {
    rows.clear();
    return kNoConnection;
  }
  MYSQL* const conn = handle.get();
  const PreparedResult r = RunPrepared(conn, sql, params, &rows);
  if (r.conn_broken) {
    MarkBroken(handle);
  }
  return r.code;
}

int MysqlClient::InsertParams(const std::string& sql,
                              const std::vector<std::string>& params,
                              uint64_t& out_insert_id) {
  BorrowedConn handle = this->Acquire(kAcquireTimeoutMs);
  if (!handle) {
    return kNoConnection;
  }
  MYSQL* const conn = handle.get();
  const PreparedResult r = RunPrepared(conn, sql, params, nullptr);
  if (r.conn_broken) {
    MarkBroken(handle);
  }
  if (r.code >= 0) {
    out_insert_id = r.insert_id;  // 失败时不动出参
  }
  return r.code;
}

}  // namespace rpc
