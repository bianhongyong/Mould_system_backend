#include "shm_ring_buffer.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace mould::comm {

namespace {

constexpr std::size_t Align8(std::size_t value) {
  return (value + 7U) & ~static_cast<std::size_t>(7U);
}

constexpr std::uint32_t kReserveRetryLimit = 16;
constexpr std::uint32_t kReserveYieldAttempts = 4;
constexpr std::uint32_t kReserveInitialBackoffUs = 4;
constexpr std::uint32_t kReserveMaxBackoffUs = 128;
constexpr std::uint32_t kCursorAdvanceFallbackReclaimInterval = 64;

}  // namespace

std::size_t ComputeRingLayoutSizeBytes(std::uint32_t slot_count, std::uint32_t consumer_capacity) {
  const std::size_t header_bytes = sizeof(RingHeader);
  const std::size_t consumer_table_bytes = sizeof(ConsumerCursor) * static_cast<std::size_t>(consumer_capacity);
  const std::size_t notification_table_bytes =
      sizeof(NotificationMappingEntry) * static_cast<std::size_t>(consumer_capacity);
  const std::size_t slot_bytes = sizeof(SlotMeta) * static_cast<std::size_t>(slot_count);
  return Align8(header_bytes + consumer_table_bytes + notification_table_bytes + slot_bytes);
}

std::optional<RingLayoutView> RingLayoutView::Initialize(
    void* base_address,
    std::size_t total_size,
    std::uint32_t slot_count,
    std::uint32_t consumer_capacity,
    std::uint64_t payload_region_bytes) {
  if (base_address == nullptr || slot_count == 0 || payload_region_bytes == 0) {
    return std::nullopt;
  }

  const std::size_t layout_size = ComputeRingLayoutSizeBytes(slot_count, consumer_capacity);
  if (total_size < layout_size + payload_region_bytes) {
    return std::nullopt;
  }

  std::memset(base_address, 0, total_size);
  auto* byte_base = static_cast<std::uint8_t*>(base_address);
  auto* header = reinterpret_cast<RingHeader*>(byte_base);

  header->magic = kRingLayoutMagic;
  header->version = kRingLayoutVersion;
  header->slot_count = slot_count;
  header->consumer_capacity = consumer_capacity;
  header->notification_capacity = consumer_capacity;
  header->payload_region_bytes = payload_region_bytes;
  header->next_sequence.store(1, std::memory_order_relaxed);
  header->reclaim_sequence.store(1, std::memory_order_relaxed);
  header->membership_generation.store(1, std::memory_order_relaxed);
  header->committed_slots.store(0, std::memory_order_relaxed);
  header->reclaim_count.store(0, std::memory_order_relaxed);
  header->membership_change_count.store(0, std::memory_order_relaxed);
  header->consumer_online_events.store(0, std::memory_order_relaxed);
  header->consumer_offline_events.store(0, std::memory_order_relaxed);
  header->producer_block_total_ns.store(0, std::memory_order_relaxed);

  auto* consumers = reinterpret_cast<ConsumerCursor*>(byte_base + sizeof(RingHeader));
  auto* notification_mappings = reinterpret_cast<NotificationMappingEntry*>(
      reinterpret_cast<std::uint8_t*>(consumers) + sizeof(ConsumerCursor) * static_cast<std::size_t>(consumer_capacity));
  auto* slots = reinterpret_cast<SlotMeta*>(
      reinterpret_cast<std::uint8_t*>(notification_mappings) +
      sizeof(NotificationMappingEntry) * static_cast<std::size_t>(consumer_capacity));
  auto* payload_region = byte_base + layout_size;

  return RingLayoutView(
      header,
      consumers,
      notification_mappings,
      slots,
      payload_region,
      total_size,
      layout_size);
}

std::optional<RingLayoutView> RingLayoutView::Attach(
    void* base_address,
    std::size_t total_size,
    std::uint16_t expected_version) {
  if (base_address == nullptr || total_size < sizeof(RingHeader)) {
    return std::nullopt;
  }

  auto* byte_base = static_cast<std::uint8_t*>(base_address);
  auto* header = reinterpret_cast<RingHeader*>(byte_base);
  if (header->magic != kRingLayoutMagic || header->version != expected_version) {
    return std::nullopt;
  }

  const std::size_t layout_size = ComputeRingLayoutSizeBytes(header->slot_count, header->consumer_capacity);
  if (total_size < layout_size + header->payload_region_bytes) {
    return std::nullopt;
  }

  auto* consumers = reinterpret_cast<ConsumerCursor*>(byte_base + sizeof(RingHeader));
  auto* notification_mappings = reinterpret_cast<NotificationMappingEntry*>(
      reinterpret_cast<std::uint8_t*>(consumers) +
      sizeof(ConsumerCursor) * static_cast<std::size_t>(header->consumer_capacity));
  auto* slots = reinterpret_cast<SlotMeta*>(
      reinterpret_cast<std::uint8_t*>(notification_mappings) +
      sizeof(NotificationMappingEntry) * static_cast<std::size_t>(header->notification_capacity));
  auto* payload_region = byte_base + layout_size;

  return RingLayoutView(
      header,
      consumers,
      notification_mappings,
      slots,
      payload_region,
      total_size,
      layout_size);
}

RingLayoutView::RingLayoutView(
    RingHeader* header,
    ConsumerCursor* consumers,
    NotificationMappingEntry* notification_mappings,
    SlotMeta* slots,
    std::uint8_t* payload_region,
    std::size_t total_size,
    std::size_t layout_size)
    : header_(header),
      consumers_(consumers),
      notification_mappings_(notification_mappings),
      slots_(slots),
      payload_region_(payload_region),
      total_size_(total_size),
      layout_size_(layout_size) {}

RingHeader* RingLayoutView::Header() const {
  return header_;
}

ConsumerCursor* RingLayoutView::Consumers() const {
  return consumers_;
}

NotificationMappingEntry* RingLayoutView::NotificationMappings() const {
  return notification_mappings_;
}

SlotMeta* RingLayoutView::Slots() const {
  return slots_;
}

std::uint8_t* RingLayoutView::PayloadRegion() const {
  return payload_region_;
}

std::size_t RingLayoutView::TotalSizeBytes() const {
  return total_size_;
}

std::size_t RingLayoutView::LayoutSizeBytes() const {
  return layout_size_;
}

bool RingLayoutView::ReserveSlot(
    std::uint32_t slot_index,
    std::uint32_t payload_size,
    std::uint64_t payload_offset,
    RingSlotReservation* out) {
  if (!IsSlotIndexValid(slot_index) || payload_size == 0 || out == nullptr || header_ == nullptr) {
    return false;
  }
  if (payload_offset + payload_size > header_->payload_region_bytes) {
    return false;
  }

  auto& slot = slots_[slot_index];
  bool acquired = false;
  std::uint32_t backoff_us = kReserveInitialBackoffUs;
  for (std::uint32_t attempt = 0; attempt < kReserveRetryLimit; ++attempt) {
    std::uint32_t expected = static_cast<std::uint32_t>(SlotState::kEmpty);
    if (slot.state.compare_exchange_strong(
            expected,
            static_cast<std::uint32_t>(SlotState::kReserved),
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      acquired = true;
      break;
    }
    if (attempt < kReserveYieldAttempts) {
      std::this_thread::yield();
      continue;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(backoff_us));
    backoff_us = (backoff_us < kReserveMaxBackoffUs / 2U) ? backoff_us * 2U : kReserveMaxBackoffUs;
  }

  if (!acquired) {
    return false;
  }

  const std::uint64_t sequence = header_->next_sequence.fetch_add(1, std::memory_order_relaxed);
  slot.payload_size = payload_size;
  slot.payload_offset = payload_offset;
  slot.sequence = sequence;

  out->sequence = sequence;
  out->payload_size = payload_size;
  out->payload_offset = payload_offset;
  return true;
}

bool RingLayoutView::CommitSlot(std::uint32_t slot_index) {
  if (!IsSlotIndexValid(slot_index)) {
    return false;
  }
  auto& slot = slots_[slot_index];
  if (slot.state.load(std::memory_order_acquire) != static_cast<std::uint32_t>(SlotState::kReserved)) {
    return false;
  }

  std::atomic_thread_fence(std::memory_order_release);
  slot.state.store(static_cast<std::uint32_t>(SlotState::kCommitted), std::memory_order_release);
  header_->committed_slots.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool RingLayoutView::TryReadCommitted(std::uint32_t slot_index, RingSlotReservation* out) const {
  if (!IsSlotIndexValid(slot_index) || out == nullptr) {
    return false;
  }
  const auto& slot = slots_[slot_index];
  if (slot.state.load(std::memory_order_acquire) != static_cast<std::uint32_t>(SlotState::kCommitted)) {
    return false;
  }

  out->sequence = slot.sequence;
  out->payload_size = slot.payload_size;
  out->payload_offset = slot.payload_offset;
  return true;
}

bool RingLayoutView::ResetSlot(std::uint32_t slot_index) {
  if (!IsSlotIndexValid(slot_index)) {
    return false;
  }

  auto& slot = slots_[slot_index];
  slot.payload_size = 0;
  slot.payload_offset = 0;
  slot.sequence = 0;
  slot.state.store(static_cast<std::uint32_t>(SlotState::kEmpty), std::memory_order_release);
  return true;
}

bool RingLayoutView::RegisterConsumer(std::uint32_t consumer_index, std::uint64_t initial_read_sequence) {
  if (header_ == nullptr || consumers_ == nullptr || initial_read_sequence == 0 ||
      consumer_index >= header_->consumer_capacity) {
    return false;
  }
  auto& consumer = consumers_[consumer_index];
  std::uint32_t expected_state = static_cast<std::uint32_t>(ConsumerState::kOffline);
  if (!consumer.state.compare_exchange_strong(
          expected_state,
          static_cast<std::uint32_t>(ConsumerState::kOnline),
          std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return false;
  }
  consumer.read_sequence.store(initial_read_sequence, std::memory_order_release);
  header_->membership_generation.fetch_add(1, std::memory_order_acq_rel);
  header_->membership_change_count.fetch_add(1, std::memory_order_relaxed);
  header_->consumer_online_events.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool RingLayoutView::UnregisterConsumer(std::uint32_t consumer_index) {
  if (header_ == nullptr || consumers_ == nullptr || consumer_index >= header_->consumer_capacity) {
    return false;
  }
  auto& consumer = consumers_[consumer_index];
  const std::uint32_t previous_state = consumer.state.exchange(
      static_cast<std::uint32_t>(ConsumerState::kOffline),
      std::memory_order_acq_rel);
  if (previous_state != static_cast<std::uint32_t>(ConsumerState::kOnline)) {
    return false;
  }
  header_->membership_generation.fetch_add(1, std::memory_order_acq_rel);
  header_->membership_change_count.fetch_add(1, std::memory_order_relaxed);
  header_->consumer_offline_events.fetch_add(1, std::memory_order_relaxed);
  (void)ReclaimCommittedSlots();
  return true;
}

bool RingLayoutView::AddConsumerOnline(std::uint32_t consumer_index) {
  if (header_ == nullptr || consumers_ == nullptr || consumer_index >= header_->consumer_capacity) {
    return false;
  }
  // New consumers start from the current ring head and do not replay historical backlog.
  const std::uint64_t initial_read_sequence = header_->next_sequence.load(std::memory_order_acquire);
  return RegisterConsumer(consumer_index, initial_read_sequence == 0 ? 1 : initial_read_sequence);
}

bool RingLayoutView::RemoveConsumerOnline(std::uint32_t consumer_index) {
  return UnregisterConsumer(consumer_index);
}

bool RingLayoutView::RecordProducerBlockDuration(std::uint64_t blocked_ns) {
  if (header_ == nullptr) {
    return false;
  }
  header_->producer_block_total_ns.fetch_add(blocked_ns, std::memory_order_relaxed);
  return true;
}

bool RingLayoutView::SetConsumerNotificationFd(std::uint32_t consumer_index, std::int32_t event_fd) {
  if (header_ == nullptr || notification_mappings_ == nullptr ||
      consumer_index >= header_->notification_capacity) {
    return false;
  }
  notification_mappings_[consumer_index].event_fd = event_fd;
  return true;
}

std::optional<std::int32_t> RingLayoutView::LoadConsumerNotificationFd(std::uint32_t consumer_index) const {
  if (header_ == nullptr || notification_mappings_ == nullptr ||
      consumer_index >= header_->notification_capacity) {
    return std::nullopt;
  }
  return notification_mappings_[consumer_index].event_fd;
}

bool RingLayoutView::AdvanceConsumerCursor(std::uint32_t consumer_index, std::uint64_t next_read_sequence) {
  if (header_ == nullptr || consumers_ == nullptr || next_read_sequence == 0 ||
      consumer_index >= header_->consumer_capacity) {
    return false;
  }
  auto& consumer = consumers_[consumer_index];
  if (consumer.state.load(std::memory_order_acquire) !=
      static_cast<std::uint32_t>(ConsumerState::kOnline)) {
    return false;
  }

  std::uint64_t current = consumer.read_sequence.load(std::memory_order_acquire);
  while (next_read_sequence >= current) {
    if (consumer.read_sequence.compare_exchange_weak(
            current,
            next_read_sequence,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      static std::atomic<std::uint64_t> cursor_advance_counter{0};
      const std::uint64_t value = cursor_advance_counter.fetch_add(1, std::memory_order_relaxed) + 1;
      if (value % kCursorAdvanceFallbackReclaimInterval == 0U) {
        (void)ReclaimCommittedSlots();
      }
      return true;
    }
  }
  return false;
}

bool RingLayoutView::CompleteConsumerSlot(
    std::uint32_t consumer_index,
    std::uint32_t slot_index,
    std::uint64_t next_read_sequence) {
  if (!AdvanceConsumerCursor(consumer_index, next_read_sequence)) {
    return false;
  }
  (void)TryReclaimSlotIfSafe(slot_index);
  return true;
}

std::optional<std::uint64_t> RingLayoutView::LoadConsumerCursor(std::uint32_t consumer_index) const {
  if (header_ == nullptr || consumers_ == nullptr || consumer_index >= header_->consumer_capacity) {
    return std::nullopt;
  }
  const auto& consumer = consumers_[consumer_index];
  if (consumer.state.load(std::memory_order_acquire) !=
      static_cast<std::uint32_t>(ConsumerState::kOnline)) {
    return std::nullopt;
  }
  return consumer.read_sequence.load(std::memory_order_acquire);
}

std::optional<std::uint64_t> RingLayoutView::MinActiveReadSequence() const {
  if (header_ == nullptr || consumers_ == nullptr) {
    return std::nullopt;
  }
  while (true) {
    const std::uint64_t generation_before = header_->membership_generation.load(std::memory_order_acquire);
    std::optional<std::uint64_t> min_read_sequence;
    for (std::uint32_t i = 0; i < header_->consumer_capacity; ++i) {
      if (consumers_[i].state.load(std::memory_order_acquire) !=
          static_cast<std::uint32_t>(ConsumerState::kOnline)) {
        continue;
      }
      const std::uint64_t candidate = consumers_[i].read_sequence.load(std::memory_order_acquire);
      if (!min_read_sequence.has_value() || candidate < *min_read_sequence) {
        min_read_sequence = candidate;
      }
    }
    const std::uint64_t generation_after = header_->membership_generation.load(std::memory_order_acquire);
    if (generation_before == generation_after) {
      return min_read_sequence;
    }
  }
}

std::uint64_t RingLayoutView::ReclaimCommittedSlots() {
  if (header_ == nullptr || slots_ == nullptr) {
    return 0;
  }

  const std::optional<std::uint64_t> min_read_sequence = MinActiveReadSequence();
  if (!min_read_sequence.has_value()) {
    return 0;
  }

  std::uint64_t reclaimed = 0;
  std::uint64_t max_reclaimed_sequence = header_->reclaim_sequence.load(std::memory_order_acquire);
  for (std::uint32_t i = 0; i < header_->slot_count; ++i) {
    RingSlotReservation slot{};
    if (!TryReadCommitted(i, &slot)) {
      continue;
    }
    if (slot.sequence < *min_read_sequence && ResetSlot(i)) {
      ++reclaimed;
      header_->reclaim_count.fetch_add(1, std::memory_order_relaxed);
      header_->committed_slots.fetch_sub(1, std::memory_order_relaxed);
      if (slot.sequence > max_reclaimed_sequence) {
        max_reclaimed_sequence = slot.sequence;
      }
    }
  }
  header_->reclaim_sequence.store(max_reclaimed_sequence, std::memory_order_release);
  return reclaimed;
}

std::optional<std::uint64_t> RingLayoutView::ConsumerLag(std::uint32_t consumer_index) const {
  if (header_ == nullptr) {
    return std::nullopt;
  }
  const std::optional<std::uint64_t> read_sequence = LoadConsumerCursor(consumer_index);
  if (!read_sequence.has_value()) {
    return std::nullopt;
  }
  const std::uint64_t next_sequence = header_->next_sequence.load(std::memory_order_acquire);
  if (next_sequence <= *read_sequence) {
    return 0;
  }
  return next_sequence - *read_sequence;
}

std::optional<std::uint64_t> RingLayoutView::MembershipGeneration() const {
  if (header_ == nullptr) {
    return std::nullopt;
  }
  return header_->membership_generation.load(std::memory_order_acquire);
}

RingHealthMetrics RingLayoutView::ObserveHealthMetrics() const {
  RingHealthMetrics metrics{};
  if (header_ == nullptr) {
    return metrics;
  }
  metrics.occupancy_slots = header_->committed_slots.load(std::memory_order_acquire);
  metrics.slot_capacity = header_->slot_count;
  metrics.producer_block_total_ns = header_->producer_block_total_ns.load(std::memory_order_acquire);
  metrics.reclaim_count = header_->reclaim_count.load(std::memory_order_acquire);
  metrics.membership_change_count = header_->membership_change_count.load(std::memory_order_acquire);
  metrics.consumer_online_events = header_->consumer_online_events.load(std::memory_order_acquire);
  metrics.consumer_offline_events = header_->consumer_offline_events.load(std::memory_order_acquire);

  for (std::uint32_t i = 0; i < header_->consumer_capacity; ++i) {
    const auto lag = ConsumerLag(i);
    if (lag.has_value() && *lag > metrics.max_consumer_lag) {
      metrics.max_consumer_lag = *lag;
    }
  }
  return metrics;
}

bool RingLayoutView::IsSlotIndexValid(std::uint32_t slot_index) const {
  return header_ != nullptr && slots_ != nullptr && slot_index < header_->slot_count;
}

bool RingLayoutView::TryReclaimSlotIfSafe(std::uint32_t slot_index) {
  if (!IsSlotIndexValid(slot_index)) {
    return false;
  }

  auto& slot = slots_[slot_index];
  if (slot.state.load(std::memory_order_acquire) != static_cast<std::uint32_t>(SlotState::kCommitted)) {
    return false;
  }

  const std::optional<std::uint64_t> min_read_sequence = MinActiveReadSequence();
  if (!min_read_sequence.has_value() || slot.sequence >= *min_read_sequence) {
    return false;
  }

  std::uint32_t expected = static_cast<std::uint32_t>(SlotState::kCommitted);
  if (!slot.state.compare_exchange_strong(
          expected,
          static_cast<std::uint32_t>(SlotState::kReserved),
          std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return false;
  }

  slot.payload_size = 0;
  slot.payload_offset = 0;
  const std::uint64_t reclaimed_sequence = slot.sequence;
  slot.sequence = 0;
  slot.state.store(static_cast<std::uint32_t>(SlotState::kEmpty), std::memory_order_release);

  std::uint64_t watermark = header_->reclaim_sequence.load(std::memory_order_acquire);
  while (reclaimed_sequence > watermark &&
         !header_->reclaim_sequence.compare_exchange_weak(
             watermark,
             reclaimed_sequence,
             std::memory_order_acq_rel,
             std::memory_order_acquire)) {
  }
  return true;
}

}  // namespace mould::comm
