#pragma once

#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mould::comm {

using ByteBuffer = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;

struct MessageEnvelope {
  std::string channel;
  std::string publisher_module;
  std::uint64_t delivery_id = 0;
  std::uint64_t sequence = 0;
  std::uint32_t retry_count = 0;
  bool maybe_duplicate = false;
  Clock::time_point timestamp = Clock::now();
  ByteBuffer payload;
};

class MessageCodec {
 public:
  virtual ~MessageCodec() = default;
  virtual ByteBuffer Encode(const MessageEnvelope& message) const = 0;
  virtual bool Decode(const ByteBuffer& bytes, MessageEnvelope* out_message) const = 0;
};

class TimerScheduler {
 public:
  using Task = std::function<void()>;
  using TimerId = std::uint64_t;

  virtual ~TimerScheduler() = default;
  virtual TimerId RegisterPeriodic(std::chrono::milliseconds interval, Task task) = 0;
  virtual void Cancel(TimerId timer_id) = 0;
  virtual void PumpDueTimers() = 0;
};

class IEventLoop {
 public:
  virtual ~IEventLoop() = default;
  virtual void RunOnce() = 0;
  virtual void Stop() = 0;
};

class IPubSubBus {
 public:
  using MessageHandler = std::function<void(const MessageEnvelope&)>;

  virtual ~IPubSubBus() = default;
  virtual bool RegisterPublisher(const std::string& module_name, const std::string& channel) = 0;
  virtual bool Subscribe(
      const std::string& module_name,
      const std::string& channel,
      MessageHandler handler) = 0;
  virtual bool Publish(
      const std::string& module_name,
      const std::string& channel,
      ByteBuffer payload) = 0;

  // Returns committed sequence on success; detailed status on failure.
  virtual absl::StatusOr<std::uint64_t> PublishWithStatus(
      const std::string& module_name,
      const std::string& channel,
      ByteBuffer payload) {
    if (Publish(module_name, channel, std::move(payload))) {
      return 0U;
    }
    return absl::UnknownError("publish failed without detailed reason");
  }
};

}  // namespace mould::comm
