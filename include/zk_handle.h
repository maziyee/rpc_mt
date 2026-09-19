#pragma once

#include <zookeeper/zookeeper.h>

#include <memory>
#include <string>

namespace rpc {

// zhandle_t 是 ZooKeeper C 客户端的不透明类型，生命周期由
// zookeeper_init / zookeeper_close 管理。
//
// 用 shared_ptr + 自定义 deleter 包起来，好处是「谁负责 zookeeper_close」
// 由引用计数决定，而不是靠调用约定：ZkHandler 和借用它的对象各持一份，
// 最后一个释放的才真正关闭连接。于是释放顺序怎么写都不会出错
// （裸指针借用的话，必须保证"先用者先释放"，写反了就会把正在用的连接关掉）。
using ZkHandle = std::shared_ptr<zhandle_t>;

// 注意：zookeeper_init 是异步的 —— 返回非空只代表 handle 创建成功，
// 真正的连接是否建立要看 zoo_state()。所以这里只负责创建，不负责等待。
inline ZkHandle MakeZkHandle(const std::string& hosts, watcher_fn watcher,
                             void* ctx, int session_timeout_ms = 30000) {
  zhandle_t* raw = zookeeper_init(hosts.c_str(), watcher, session_timeout_ms,
                                  nullptr, ctx, 0);
  if (raw == nullptr) {
    return nullptr;
  }
  return ZkHandle(raw, [](zhandle_t* handle) {
    if (handle != nullptr) {
      zookeeper_close(handle);
    }
  });
}

}  // namespace rpc
