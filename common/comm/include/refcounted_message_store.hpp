#pragma once

#include "interfaces.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace mould::comm {

class RefCountedMessageStore {
 public:
  struct Handle {
    std::size_t slot = 0;
    std::uint32_t generation = 0;
  };

  explicit RefCountedMessageStore(std::size_t capacity);

  std::optional<Handle> Reserve();
  bool WritePayload(const Handle& handle, ByteBuffer payload);
  bool CommitVisible(const Handle& handle, std::uint32_t initial_ref_count);
  std::optional<ByteBuffer> AcquireVisible(const Handle& handle);
  bool Release(const Handle& handle);
  std::size_t SweepExpired(std::chrono::milliseconds max_age);

 private:
  struct SlotState {
    std::mutex mutex;
    std::atomic<std::uint32_t> generation{1};
    std::atomic<std::uint32_t> ref_count{0};
    std::atomic<bool> visible{false};
    bool reserved = false;
    ByteBuffer payload;
    Clock::time_point reserve_ts = Clock::now();
  };

  bool MatchHandle(const Handle& handle, const SlotState& slot) const;
  void ResetSlotLocked(SlotState* slot);

  std::vector<SlotState> slots_;
};

}  // namespace mould::comm
