#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>

namespace mould::comm {

class CallbackQueue {
 public:
  using Callback = std::function<void()>;

  void Push(Callback callback);
  std::size_t DrainAll();
  std::size_t WaitAndDrain(std::chrono::microseconds timeout);

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<Callback> queue_;
};

}  // namespace mould::comm
