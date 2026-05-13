#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>

namespace mould {

/// 简单的线程池，支持回调模式。
///
/// 通过 Submit() 投递任务，worker 线程取出并执行。
/// "回调模式" 的体现：调用方将工作 + 完成回调打包为一个 callable 提交，
/// worker 执行完毕后自然触发回调。
class ThreadPool {
 public:
  explicit ThreadPool(size_t num_threads = std::thread::hardware_concurrency())
      : stop_(false) {
    start_workers(num_threads > 0 ? num_threads : 1);
  }

  ~ThreadPool() { shutdown(); }

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  /// 投递一个任务到队列。任务会在某个 worker 线程中执行。
  /// @param task  可调用对象（void()），通常为 lambda
  void Submit(std::function<void()> task) {
    if (stop_.load(std::memory_order_relaxed)) return;

    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.push(std::move(task));
    }
    cv_.notify_one();
  }

  /// 剩余队列中的任务数量（主要用于测试/监控）。
  size_t PendingTasks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
  }

 private:
  void start_workers(size_t count) {
    workers_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      workers_.emplace_back([this] { worker_loop(); });
    }
  }

  void shutdown() {
    stop_.store(true, std::memory_order_relaxed);
    cv_.notify_all();
    for (auto& t : workers_) {
      if (t.joinable()) t.join();
    }
  }

  void worker_loop() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] {
          return stop_.load(std::memory_order_relaxed) || !tasks_.empty();
        });
        if (stop_.load(std::memory_order_relaxed) && tasks_.empty()) break;
        task = std::move(tasks_.front());
        tasks_.pop();
      }
      task();
    }
  }

  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> tasks_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::atomic<bool> stop_;
};

}  // namespace mould
