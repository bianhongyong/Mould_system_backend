#pragma once

#include <atomic>
#include <string>

#include "gate/handler/ihandler.hpp"

namespace mould {
namespace gate {
namespace handler {

/// 纯内存计数器 handler — 用于 muduo 并发性能压测。
///
/// 每次请求原子自增计数器并返回当前值，不涉及 SHM/OSS/磁盘 I/O，
/// 最大限度消除业务逻辑对并发性能测试的干扰。
class CounterHandler : public IHandler {
 public:
  CounterHandler();

  uint64_t Count() const { return counter_.load(); }
  void Reset() { counter_.store(0); }

 protected:
  ParseResult DoParse(http::RequestView& view) override;
  ProcessResult DoProcess(const ParseResult& input) override;

 private:
  std::atomic<uint64_t> counter_{0};
};

}  // namespace handler
}  // namespace gate
}  // namespace mould
