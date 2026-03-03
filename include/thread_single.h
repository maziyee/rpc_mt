#pragma once

#include <condition_variable>
#include <mutex>
#include <thread>

#include "thread_pool.h"

namespace meeting_ctrl {
class ThreadSingle {
 public:
  static bool Init(size_t threads = std::thread::hardware_concurrency());
  static bool Init(const ThreadStruct& config);

  static ThreadPool& GetInstance();
  template <class F, class... Args>
  static auto Enqueue(TaskPriority pri, F&& f, Args&&... args)
      -> std::future<typename std::invoke_result<F, Args...>::type> {
    return GetInstance().Enqueue(pri, std::forward<F>(f),
                                 std::forward<Args>(args)...);
  }

  static ThreadPool::Stats GetStats();
  static size_t GetTaskSize();
  static size_t GetThreadPoolSize();
  static bool Pause();
  static bool Resume();
  static bool ShutDown();
  static ThreadStatus GetStatus();
  ThreadSingle(const ThreadSingle&) = delete;
  ThreadSingle& operator=(const ThreadSingle&) = delete;

 private:
  ThreadSingle() = default;

 private:
  static std::unique_ptr<ThreadPool> thread_instance_;
  static std::mutex mutex_;
};
}  // namespace meeting_ctrl