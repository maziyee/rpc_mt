#include "thread_single.h"

std::unique_ptr<meeting_ctrl::ThreadPool> meeting_ctrl::ThreadSingle::thread_instance_;
std::mutex meeting_ctrl::ThreadSingle::mutex_;

bool meeting_ctrl::ThreadSingle::Init(size_t threads) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (thread_instance_) {
    return false;
  }
  thread_instance_ = std::make_unique<ThreadPool>(threads);
  return true;
}

bool meeting_ctrl::ThreadSingle::Init(const ThreadStruct& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (thread_instance_) {
    return false;
  }
  thread_instance_ = std::make_unique<ThreadPool>(config);
  return true;
}

meeting_ctrl::ThreadPool& meeting_ctrl::ThreadSingle::GetInstance() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!thread_instance_) {
    LOG_ERROR("ThreadSingle GetInstance fail , instance is null");
    Init();
  }
  return *thread_instance_;
}

meeting_ctrl::ThreadPool::Stats meeting_ctrl::ThreadSingle::GetStats() {
  return GetInstance().GetStats();
}

size_t meeting_ctrl::ThreadSingle::GetTaskSize() {
  return GetInstance().GetTasksSize();
}

size_t meeting_ctrl::ThreadSingle::GetThreadPoolSize() {
  return GetInstance().GetActiveThreads();
}

bool meeting_ctrl::ThreadSingle::Pause() {
  GetInstance().Pause();
  if (GetInstance().GetState() == meeting_ctrl::ThreadStatus::kStopped) {
    return true;
  }
  return false;
}

bool meeting_ctrl::ThreadSingle::Resume() {
  GetInstance().Resume();
  if (GetInstance().GetState() == meeting_ctrl::ThreadStatus::kRunning) {
    return true;
  }
  return false;
}

bool meeting_ctrl::ThreadSingle::ShutDown() { return GetInstance().ShutDown(); }

meeting_ctrl::ThreadStatus meeting_ctrl::ThreadSingle::GetStatus() {
  return GetInstance().GetState();
}
