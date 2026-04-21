#include "callback_queue.hpp"

#include <utility>

namespace mould::comm {

void CallbackQueue::Push(Callback callback) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(std::move(callback));
  }
  cv_.notify_one();
}

std::size_t CallbackQueue::DrainAll() {
  std::queue<Callback> local_queue;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    local_queue.swap(queue_);
  }

  std::size_t drained = 0;
  while (!local_queue.empty()) {
    Callback callback = std::move(local_queue.front());
    local_queue.pop();
    if (callback) {
      callback();
    }
    ++drained;
  }
  return drained;
}

std::size_t CallbackQueue::WaitAndDrain(std::chrono::microseconds timeout) {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, timeout, [this]() { return !queue_.empty(); });
  }
  return DrainAll();
}

}  // namespace mould::comm
