#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#include "log_manager.h"

namespace meeting_ctrl {
enum class TaskPriority { kHIGH, kNORMAL, kLOW };
enum class ThreadStatus { kRunning, KPuase, kShuttingDown, kStopped };
struct ThreadStruct {
  ThreadStruct() = default;
  ThreadStruct(size_t core_threads, size_t max_threads, size_t keep_alive_time,
               size_t queue_size = 0)
      : core_threads(core_threads),
        max_threads(max_threads),
        queue_size(queue_size) {
    this->keep_alive_time = std::chrono::seconds(keep_alive_time);
  };
  size_t core_threads = std::thread::hardware_concurrency();
  size_t max_threads = std::thread::hardware_concurrency() * 2;
  std::chrono::seconds keep_alive_time{60};
  size_t queue_size = 1000;
};

class TaskWrapper {
 public:
  TaskWrapper(std::function<void()>&& task, TaskPriority priority)
      : task_(std::move(task)), priority_(priority) {}
  TaskWrapper() : priority_(TaskPriority::kNORMAL) {}
  bool vaild() { return static_cast<bool>(task_); }
  void run() {
    if (vaild()) {
      task_();
    }
  }

  bool operator<(const TaskWrapper& other) const {
    return priority_ > other.priority_;
  }

 private:
 private:
  std::function<void()> task_;
  TaskPriority priority_;
};

class ThreadPool {
 public:
  struct Stats {
    size_t active_threads = 0;
    size_t completed_tasks = 0;
    size_t fail_tasks = 0;
    double active_time_ms = 0;
  };
  ThreadPool(const ThreadStruct& thread_struct);
  ThreadPool(size_t thread_nums = std::thread::hardware_concurrency());

  template <typename F, typename... Args>
  auto Enqueue(TaskPriority prior, F&& f, Args&&... args)
      -> std::future<typename std::invoke_result<F, Args...>::type>;
  ~ThreadPool();
  Stats GetStats();

  size_t GetTasksSize();

  size_t GetActiveThreads() const;
  size_t GetCompletedTasks() const;
  size_t GetFailTasks() const;
  double GetActiveTimeMs() const;

  bool ShutDown();
  bool Stop_now();

  void Pause();

  void Resume();

  ThreadStatus GetState() const { return this->state_.load(); }

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

 private:
  void WorkerThread();
  void UpdateThreadNums();

 private:
  std::vector<std::thread> workers;
  std::atomic<size_t> current_thread_nums{0};
  std::vector<std::chrono::steady_clock::time_point> thread_active_time;

  std::priority_queue<TaskWrapper> task_queue;

  ThreadStruct thread_config_;

  std::mutex queue_mutex_;
  std::condition_variable get_task_condition;
  std::atomic<bool> stop_{false};
  std::condition_variable not_full_condition;

  std::atomic<ThreadStatus> state_{ThreadStatus::kRunning};

  mutable std::mutex stats_mutex_;
  std::atomic<size_t> active_threads_{0};
  std::atomic<size_t> completed_tasks_{0};
  std::atomic<size_t> fail_tasks_{0};
  double active_time_ms_{0};
};

template <typename F, typename... Args>
inline auto ThreadPool::Enqueue(TaskPriority prior, F&& f, Args&&... args)
    -> std::future<typename std::invoke_result<F, Args...>::type> {
  using return_type = typename std::invoke_result<F, Args...>::type;
  auto task = std::make_shared<std::packaged_task<return_type()>>(
      std::bind(std::forward<F>(f), std::forward<Args>(args)...));

  std::future<return_type> res = task->get_future();
  {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    if (this->stop_) {
      return std::future<return_type>{};
    }
    if (this->thread_config_.queue_size > 0) {
      this->not_full_condition.wait(lock, [this]() {
        return this->task_queue.size() < this->thread_config_.queue_size ||
               this->stop_;
      });
      if (this->stop_) {
        return std::future<return_type>{};
      }
    }
    this->task_queue.emplace([task] { (*task)(); }, prior);
    std::string pri_str =
        prior == TaskPriority::kHIGH
            ? "HIGH"
            : (prior == TaskPriority::kNORMAL ? "NORMAL" : "LOW");
    LOG_INFO("Enqueue task, priority: {}, task_queue size: {}", pri_str.c_str(),
             this->task_queue.size());
    if (this->task_queue.size() > this->thread_config_.core_threads &&
        this->current_thread_nums < this->thread_config_.max_threads) {
      this->UpdateThreadNums();
    }
  }
  get_task_condition.notify_one();
  return res;
}

}  // namespace meeting_ctrl