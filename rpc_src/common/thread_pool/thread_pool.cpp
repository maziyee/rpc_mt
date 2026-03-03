#include "thread_pool.h"

meeting_ctrl::ThreadPool::ThreadPool(const ThreadStruct& thread_struct)
    : thread_config_(thread_struct),
      active_threads_(0),
      active_time_ms_(0),
      completed_tasks_(0),
      current_thread_nums(0),
      fail_tasks_(0),
      stop_(false),
      state_(ThreadStatus::kRunning) {
  this->workers.resize(thread_config_.max_threads);
  this->thread_active_time.resize(thread_config_.max_threads);
  for (int i = 0; i < thread_config_.core_threads; i++) {
    this->workers[i] = std::thread(&ThreadPool::WorkerThread, this);
    this->thread_active_time[i] = std::chrono::steady_clock::now();
  }
  this->current_thread_nums = thread_config_.core_threads;
  LOG_INFO("ThreadPool init success , dynamic");
}

meeting_ctrl::ThreadPool::ThreadPool(size_t thread_nums)
    : ThreadPool(ThreadStruct(thread_nums, thread_nums, 10000)) {
  LOG_INFO("ThreadPool init success , static");
}

meeting_ctrl::ThreadPool::~ThreadPool() {
  if (this->state_ != ThreadStatus::kStopped) {
    this->ShutDown();
  }
}

meeting_ctrl::ThreadPool::Stats meeting_ctrl::ThreadPool::GetStats() {
  meeting_ctrl::ThreadPool::Stats res;
  res.active_threads = this->active_threads_.load();
  res.active_time_ms = this->active_time_ms_;
  res.completed_tasks = this->completed_tasks_.load();
  res.fail_tasks = this->fail_tasks_.load();
  return res;
}

size_t meeting_ctrl::ThreadPool::GetTasksSize() {
  return this->task_queue.size();
}

size_t meeting_ctrl::ThreadPool::GetActiveThreads() const {
  return this->active_threads_.load();
}

size_t meeting_ctrl::ThreadPool::GetCompletedTasks() const {
  return this->completed_tasks_.load();
}

size_t meeting_ctrl::ThreadPool::GetFailTasks() const {
  return this->fail_tasks_.load();
}

double meeting_ctrl::ThreadPool::GetActiveTimeMs() const {
  return this->active_time_ms_;
}

bool meeting_ctrl::ThreadPool::ShutDown() {
  ThreadStatus except = ThreadStatus::kRunning;
  if (!this->state_.compare_exchange_strong(except,
                                            ThreadStatus::kShuttingDown)) {
    return this->state_ == ThreadStatus::kStopped;
  }
  this->get_task_condition.notify_all();
  {
    std::unique_lock<std::mutex> lock(this->queue_mutex_);
    bool tasks_complete = this->get_task_condition.wait_for(
        lock, this->thread_config_.keep_alive_time, [this] {
          return this->task_queue.empty() && this->active_threads_ == 0;
        });
    if (!tasks_complete) {
      LOG_ERROR("ThreadPool ShutDown fail , active_threads: {}",
                this->active_threads_.load());
      this->Stop_now();
      return false;
    }
    state_ = ThreadStatus::kStopped;
    this->stop_ = true;
  }
  this->get_task_condition.notify_all();
  for (auto& worker : this->workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  LOG_INFO("ThreadPool ShutDown success");
  return true;
}

bool meeting_ctrl::ThreadPool::Stop_now() {
  {
    std::unique_lock<std::mutex> lock(this->queue_mutex_);
    this->state_ = ThreadStatus::kStopped;
    this->stop_ = true;
    while (!this->task_queue.empty()) {
      this->task_queue.pop();
    }
  }
  this->get_task_condition.notify_all();
  for (std::thread& worker : workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  LOG_INFO("STOP NOW");
  return true;
}

void meeting_ctrl::ThreadPool::Pause() {
  auto except = ThreadStatus::kRunning;
  if (!this->state_.compare_exchange_strong(except, ThreadStatus::KPuase)) {
    get_task_condition.notify_all();
  }
}

void meeting_ctrl::ThreadPool::Resume() {
  auto except = ThreadStatus::KPuase;
  if (!this->state_.compare_exchange_strong(except, ThreadStatus::kRunning)) {
    get_task_condition.notify_all();
  }
}

void meeting_ctrl::ThreadPool::WorkerThread() {
  std::thread::id thread_id = std::this_thread::get_id();
  std::ostringstream oss;
  std::string thread_id_str = oss.str();

  auto last_active_time = std::chrono::steady_clock::now();

  while (true) {
    TaskWrapper task{};
    {
      std::unique_lock<std::mutex> lock(this->queue_mutex_);
      auto wait_time_out = this->thread_config_.keep_alive_time;
      bool has_task = get_task_condition.wait_for(lock, wait_time_out, [this] {
        return this->stop_ ||
               (!this->task_queue.empty() &&
                this->state_ == ThreadStatus::kRunning) ||
               (!this->task_queue.empty() &&
                this->state_ == ThreadStatus::kShuttingDown);
      });
      if (!has_task &&
          current_thread_nums.load() > thread_config_.core_threads) {
        auto now = std::chrono::steady_clock::now();
        auto active_time = now - last_active_time;
        if (active_time > thread_config_.keep_alive_time) {
          this->current_thread_nums--;
          LOG_INFO("ThreadPool thread {} exit", thread_id_str);
          return;
        }
      }
      if ((this->stop_ || this->state_ == ThreadStatus::kStopped) &&
          this->task_queue.empty()) {
        this->current_thread_nums--;
        LOG_INFO("ThreadPool thread {} exit", thread_id_str);
        return;
      }
      if (this->state_ == ThreadStatus::KPuase) {
        continue;
      }
      if (!this->task_queue.empty()) {
        task = std::move(const_cast<TaskWrapper&>(task_queue.top()));
        this->task_queue.pop();
        last_active_time = std::chrono::steady_clock::now();
        LOG_INFO("ThreadPool thread {} get task  , queue size: {}",
                 thread_id_str, task_queue.size());
        if (this->thread_config_.queue_size > 0 &&
            task_queue.size() < this->thread_config_.queue_size) {
          this->not_full_condition.notify_one();
        }
      }
    }
    if (task.vaild()) {
      auto start_time = std::chrono::steady_clock::now();
      this->active_threads_++;
      try {
        task.run();
        this->completed_tasks_++;
        auto end_time = std::chrono::steady_clock::now();
        auto dururtion = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time);
        LOG_INFO(
            "ThreadPool {} , finish a task , use time : {} , complete task "
            "nums : {}",
            thread_id_str, dururtion, this->completed_tasks_.load());

      } catch (std::exception& e) {
        this->fail_tasks_++;
        auto end_time = std::chrono::steady_clock::now();
        auto dururtion = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time);
        LOG_ERROR(
            "ThreadPool {} , fail a task , use time : {} , fail task "
            "nums : {} , fail info : {}",
            thread_id_str, dururtion, this->fail_tasks_.load(), e.what());
      }
      auto end_time = std::chrono::steady_clock::now();
      auto dururtion = std::chrono::duration_cast<std::chrono::milliseconds>(
          end_time - start_time);
      {
        std::lock_guard<std::mutex> lock(this->stats_mutex_);
        this->active_time_ms_ += dururtion.count();
      }
      this->active_threads_--;
      if (this->state_ == ThreadStatus::kShuttingDown &&
          this->active_threads_ == 0 && this->task_queue.empty()) {
        this->get_task_condition.notify_all();
      }
    }
  }
}

void meeting_ctrl::ThreadPool::UpdateThreadNums() {
  size_t new_thread_nums = this->current_thread_nums++;
  if (new_thread_nums < this->thread_active_time.size()) {
    this->thread_active_time[new_thread_nums] =
        std::chrono::steady_clock::now();
  }
  try {
    workers.emplace_back(&ThreadPool::WorkerThread, this);
  } catch (std::exception& e) {
    this->current_thread_nums--;
    LOG_ERROR("the thread create failed");
  }
}
