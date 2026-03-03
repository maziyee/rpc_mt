#include <chrono>
#include <iostream>
#include <thread>

#include "log_manager.h"
#include "thread_pool.h"
#include "thread_single.h"

void TestBasicTask() {
  LOG_INFO("TestBasicTask");
  meeting_ctrl::ThreadStruct config(2, 2, 10);
  meeting_ctrl::ThreadSingle::Init(config);
  auto res = meeting_ctrl::ThreadSingle::GetInstance().Enqueue(
      meeting_ctrl::TaskPriority::kHIGH, []() {
        LOG_INFO("TestBasicTask");
        std::this_thread::sleep_for(std::chrono::seconds(10));
        return 42;
      });
  int result = res.get();
  LOG_INFO("TestBasicTask result: {}", result);
}

void TestPriority() {
  LOG_INFO("TestPriority");
  std::mutex pri_mut;
  std::vector<int> res;
  for (int i = 0; i < 10; i++) {
    meeting_ctrl::ThreadSingle::GetInstance().Enqueue(
        meeting_ctrl::TaskPriority::kLOW, [&, i]() {
          LOG_INFO("TestPriority LOW {}", i);
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          std::lock_guard<std::mutex> lock(pri_mut);
          res.push_back(i);
        });
  }

  meeting_ctrl::ThreadSingle::GetInstance().Enqueue(
      meeting_ctrl::TaskPriority::kHIGH, [=, &pri_mut, &res]() {
        std::lock_guard<std::mutex> lock(pri_mut);
        LOG_INFO("TestPriority HIGH {}", 42);
        res.push_back(42);
      });
  meeting_ctrl::ThreadSingle::GetInstance().ShutDown();
  for (auto i : res) {
    LOG_INFO("TestPriority res {}", i);
  }
}

int main() {
  TestBasicTask();
  TestPriority();
}