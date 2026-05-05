#include "refcounted_message_store.hpp"

namespace mould::comm {

RefCountedMessageStore::RefCountedMessageStore(std::size_t capacity) : slots_(capacity) {}

std::optional<RefCountedMessageStore::Handle> RefCountedMessageStore::Reserve() {
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    SlotState& slot = slots_[i];
    std::lock_guard<std::mutex> lock(slot.mutex);
    if (slot.reserved || slot.visible.load(std::memory_order_acquire)) {
      continue;
    }
    slot.reserved = true;
    slot.reserve_ts = Clock::now();
    slot.ref_count.store(0, std::memory_order_release);
    return Handle{.slot = i, .generation = slot.generation.load(std::memory_order_acquire)};
  }
  return std::nullopt;
}

bool RefCountedMessageStore::WritePayload(const Handle& handle, ByteBuffer payload) {
  if (handle.slot >= slots_.size()) {
    return false;
  }

  SlotState& slot = slots_[handle.slot];
  std::lock_guard<std::mutex> lock(slot.mutex);
  if (!MatchHandle(handle, slot) || !slot.reserved || slot.visible.load(std::memory_order_acquire)) {
    return false;
  }
  slot.payload = std::move(payload);
  return true;
}

bool RefCountedMessageStore::CommitVisible(const Handle& handle, std::uint32_t initial_ref_count) {
  if (handle.slot >= slots_.size() || initial_ref_count == 0) {
    return false;
  }

  SlotState& slot = slots_[handle.slot];
  std::lock_guard<std::mutex> lock(slot.mutex);
  if (!MatchHandle(handle, slot) || !slot.reserved || slot.payload.empty()) {
    return false;
  }

  slot.ref_count.store(initial_ref_count, std::memory_order_release);
  slot.visible.store(true, std::memory_order_release);
  slot.reserved = false;
  return true;
}

std::optional<ByteBuffer> RefCountedMessageStore::AcquireVisible(const Handle& handle) {
  if (handle.slot >= slots_.size()) {
    return std::nullopt;
  }

  SlotState& slot = slots_[handle.slot];
  std::lock_guard<std::mutex> lock(slot.mutex);
  if (!MatchHandle(handle, slot) || !slot.visible.load(std::memory_order_acquire)) {
    return std::nullopt;
  }
  slot.ref_count.fetch_add(1, std::memory_order_acq_rel);
  return slot.payload;
}

bool RefCountedMessageStore::Release(const Handle& handle) {
  if (handle.slot >= slots_.size()) {
    return false;
  }

  SlotState& slot = slots_[handle.slot];
  std::lock_guard<std::mutex> lock(slot.mutex);
  if (!MatchHandle(handle, slot) || !slot.visible.load(std::memory_order_acquire)) {
    return false;
  }

  const std::uint32_t previous = slot.ref_count.fetch_sub(1, std::memory_order_acq_rel);
  if (previous == 0) {
    slot.ref_count.store(0, std::memory_order_release);
    return false;
  }
  if (previous == 1) {
    ResetSlotLocked(&slot);
  }
  return true;
}

std::size_t RefCountedMessageStore::SweepExpired(std::chrono::milliseconds max_age) {
  const Clock::time_point now = Clock::now();
  std::size_t reclaimed = 0;
  for (SlotState& slot : slots_) {
    std::lock_guard<std::mutex> lock(slot.mutex);
    const bool stale_reservation = slot.reserved && (now - slot.reserve_ts) > max_age;
    const bool stale_visible = slot.visible.load(std::memory_order_acquire) &&
        (slot.ref_count.load(std::memory_order_acquire) == 0) &&
        (now - slot.reserve_ts) > max_age;
    if (!stale_reservation && !stale_visible) {
      continue;
    }
    ResetSlotLocked(&slot);
    ++reclaimed;
  }
  return reclaimed;
}

bool RefCountedMessageStore::MatchHandle(const Handle& handle, const SlotState& slot) const {
  return handle.generation == slot.generation.load(std::memory_order_acquire);
}

void RefCountedMessageStore::ResetSlotLocked(SlotState* slot) {
  slot->payload.clear();
  slot->ref_count.store(0, std::memory_order_release);
  slot->visible.store(false, std::memory_order_release);
  slot->reserved = false;
  slot->generation.fetch_add(1, std::memory_order_acq_rel);
}

}  // namespace mould::comm
