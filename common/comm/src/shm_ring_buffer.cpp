#include "shm_ring_buffer.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <glog/logging.h>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace mould::comm {

namespace {

constexpr std::size_t Align8(std::size_t value) {
  return (value + 7U) & ~static_cast<std::size_t>(7U);
}

constexpr std::uint32_t kCursorAdvanceFallbackReclaimInterval = 64;

constexpr std::uint64_t PackSlotControl(SlotState state, std::uint64_t sequence) {
  return (sequence << 32U) | static_cast<std::uint64_t>(static_cast<std::uint32_t>(state));
}

constexpr SlotState UnpackSlotState(std::uint64_t control) {
  return static_cast<SlotState>(static_cast<std::uint32_t>(control & 0xFFFFFFFFULL));
}

constexpr std::uint64_t UnpackSlotSequence(std::uint64_t control) {
  return control >> 32U;
}

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
  header->next_sequence.store(1, std::memory_order_seq_cst);
  header->reclaim_sequence.store(1, std::memory_order_seq_cst);
  header->membership_generation.store(1, std::memory_order_seq_cst);
  header->committed_slots.store(0, std::memory_order_seq_cst);
  header->reclaim_count.store(0, std::memory_order_seq_cst);
  header->membership_change_count.store(0, std::memory_order_seq_cst);
  header->consumer_online_events.store(0, std::memory_order_seq_cst);
  header->consumer_offline_events.store(0, std::memory_order_seq_cst);
  header->producer_block_total_ns.store(0, std::memory_order_seq_cst);
  header->producer_payload_slot_ticket.store(0, std::memory_order_seq_cst);

  auto* consumers = reinterpret_cast<ConsumerCursor*>(byte_base + sizeof(RingHeader));
  auto* notification_mappings = reinterpret_cast<NotificationMappingEntry*>(
      reinterpret_cast<std::uint8_t*>(consumers) + sizeof(ConsumerCursor) * static_cast<std::size_t>(consumer_capacity));
  auto* slots = reinterpret_cast<SlotMeta*>(
      reinterpret_cast<std::uint8_t*>(notification_mappings) +
      sizeof(NotificationMappingEntry) * static_cast<std::size_t>(consumer_capacity));
  auto* payload_region = byte_base + layout_size;

  RingLayoutView view(
      header,
      consumers,
      notification_mappings,
      slots,
      payload_region,
      total_size,
      layout_size);

  std::vector<int> opened;
  opened.reserve(consumer_capacity);
  for (std::uint32_t i = 0; i < consumer_capacity; ++i) {
    const int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0 || !view.SetConsumerNotificationFd(i, fd)) {
      for (const int cleanup_fd : opened) {
        (void)close(cleanup_fd);
      }
      return std::nullopt;
    }
    opened.push_back(fd);
  }
  return view;
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

bool RingLayoutView::ReservePublishSlotRingOrdered(
    std::uint32_t payload_size,
    RingSlotReservation* out,
    std::uint32_t* out_slot_index) {
  if (payload_size == 0 || out == nullptr || out_slot_index == nullptr || header_ == nullptr || slots_ == nullptr) {
    return false;
  }
  const std::uint32_t slot_count = header_->slot_count;
  if (slot_count == 0) {
    return false;
  }
  const std::size_t payload_capacity = static_cast<std::size_t>(header_->payload_region_bytes);
  const std::size_t per_slot_capacity =
      payload_capacity / static_cast<std::size_t>(std::max<std::uint32_t>(slot_count, 1U));
  if (per_slot_capacity == 0 || static_cast<std::uint64_t>(payload_size) > static_cast<std::uint64_t>(per_slot_capacity)) {
    return false;
  }

  const std::uint64_t s = header_->next_sequence.load(std::memory_order_seq_cst);
  if (s == 0) {
    LOG(ERROR) << "Sequence is 0";
    return false;
  }
  const std::uint32_t slot_index =
      static_cast<std::uint32_t>((s - 1U) % static_cast<std::uint64_t>(slot_count));
  const std::uint64_t payload_offset =
      static_cast<std::uint64_t>(slot_index) * static_cast<std::uint64_t>(per_slot_capacity);
  if (payload_offset + static_cast<std::uint64_t>(payload_size) > header_->payload_region_bytes) {
    LOG(ERROR) << "payload_offset + payload_size > header_->payload_region_bytes";
    return false;
  }

  auto& slot = slots_[slot_index];
  std::uint64_t control = slot.state_sequence.load(std::memory_order_seq_cst);
  if (UnpackSlotState(control) != SlotState::kEmpty) {
    return false;
  }
  const std::uint64_t desired_control = PackSlotControl(SlotState::kReserved, s);
  if (!slot.state_sequence.compare_exchange_strong(
          control,
          desired_control,
          std::memory_order_seq_cst,
          std::memory_order_seq_cst)) {
    return false;
  }

  std::uint64_t expected_seq = s;
  if (!header_->next_sequence.compare_exchange_strong(
          expected_seq,
          s + 1U,
          std::memory_order_seq_cst,
          std::memory_order_seq_cst)) {
    slot.state_sequence.store(PackSlotControl(SlotState::kEmpty, 0), std::memory_order_seq_cst);
    return false;
  }

  slot.payload_size = payload_size;
  slot.payload_offset = payload_offset;
  out->sequence = s;
  out->payload_size = payload_size;
  out->payload_offset = payload_offset;
  *out_slot_index = slot_index;
  //LOG(INFO) << "ReservePublishSlotRingOrdered success, slot_index=" << slot_index << " sequence=" << s;
  return true;
}

bool RingLayoutView::CommitSlot(std::uint32_t slot_index) {
  if (!IsSlotIndexValid(slot_index)) {
    return false;
  }
  auto& slot = slots_[slot_index];
  std::uint64_t expected_control = slot.state_sequence.load(std::memory_order_seq_cst);
  if (UnpackSlotState(expected_control) != SlotState::kReserved) {
    return false;
  }
  const std::uint64_t committed_control =
      PackSlotControl(SlotState::kCommitted, UnpackSlotSequence(expected_control));
  if (!slot.state_sequence.compare_exchange_strong(
          expected_control,
          committed_control,
          std::memory_order_seq_cst,
          std::memory_order_seq_cst)) {
    return false;
  }

  std::atomic_thread_fence(std::memory_order_seq_cst);
  header_->committed_slots.fetch_add(1, std::memory_order_seq_cst);
  return true;
}

bool RingLayoutView::TryReadCommitted(std::uint32_t slot_index, RingSlotReservation* out) const {
  if (!IsSlotIndexValid(slot_index) || out == nullptr) {
    return false;
  }
  const auto& slot = slots_[slot_index];
  constexpr int kMaxAttempts = 16;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    const std::uint64_t control1 = slot.state_sequence.load(std::memory_order_seq_cst);
    if (UnpackSlotState(control1) != SlotState::kCommitted) {
      return false;
    }
    const std::uint64_t sequence = UnpackSlotSequence(control1);
    const std::uint32_t payload_size = slot.payload_size;
    const std::uint64_t payload_offset = slot.payload_offset;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const std::uint64_t control2 = slot.state_sequence.load(std::memory_order_seq_cst);
    if (control2 != control1) {
      continue;
    }
    out->sequence = sequence;
    out->payload_size = payload_size;
    out->payload_offset = payload_offset;
    return true;
  }
  return false;
}

bool RingLayoutView::TryReadNextForConsumer(
    std::uint32_t consumer_index,
    RingSlotReservation* out,
    std::uint32_t* out_slot_index) const {
  if (out == nullptr || out_slot_index == nullptr || header_ == nullptr || consumers_ == nullptr || slots_ == nullptr ||
      consumer_index >= header_->consumer_capacity) {
    return false;
  }
  const auto& consumer = consumers_[consumer_index];
  if (consumer.state.load(std::memory_order_seq_cst) != static_cast<std::uint32_t>(ConsumerState::kOnline)) {
    return false;
  }
  const std::uint64_t expected_sequence = consumer.read_sequence.load(std::memory_order_seq_cst);
  if (expected_sequence == 0) {
    return false;
  }

  const std::uint32_t slot_count = header_->slot_count;
  if (slot_count == 0) {
    return false;
  }
  const std::uint32_t slot_index =
      static_cast<std::uint32_t>((expected_sequence - 1U) % static_cast<std::uint64_t>(slot_count));
  if (!TryReadCommitted(slot_index, out)) {
    //LOG(ERROR) << "TryReadCommitted failed, slot_index=" << slot_index << " expected_sequence=" << expected_sequence;
    return false;
  }
  if (out->sequence != expected_sequence) {
    VLOG(2) << "TryReadNextForConsumer: sequence mismatch (retry), slot_index=" << slot_index
            << " expected_sequence=" << expected_sequence << " out->sequence=" << out->sequence;
    *out = RingSlotReservation{};
    return false;
  }
  *out_slot_index = slot_index;
  return true;
}

bool RingLayoutView::ResetSlot(std::uint32_t slot_index) {
  if (!IsSlotIndexValid(slot_index)) {
    return false;
  }

  auto& slot = slots_[slot_index];
  slot.payload_size = 0;
  slot.payload_offset = 0;
  slot.state_sequence.store(PackSlotControl(SlotState::kEmpty, 0), std::memory_order_seq_cst);
  return true;
}

bool RingLayoutView::RegisterConsumer(std::uint32_t consumer_index, std::uint64_t initial_read_sequence) {
  if (header_ == nullptr || consumers_ == nullptr || initial_read_sequence == 0 ||
      consumer_index >= header_->consumer_capacity) {
    return false;
  }
  auto& consumer = consumers_[consumer_index];
  std::uint32_t expected_offline = static_cast<std::uint32_t>(ConsumerState::kOffline);
  if (!consumer.state.compare_exchange_strong(
          expected_offline,
          static_cast<std::uint32_t>(ConsumerState::kRegistering),
          std::memory_order_seq_cst,
          std::memory_order_seq_cst)) {
    return false;
  }
  // 必须在发布 `kOnline` 之前写入游标；否则 `MinActiveReadSequence` 可能在观测到 online 后仍读到旧游标，
  // `TryReclaimSlotIfSafe` 会误判全局最慢游标已越过本槽序号，从而提前回收槽位并覆盖未交付消息。
  consumer.read_sequence.store(initial_read_sequence, std::memory_order_seq_cst);
  std::uint32_t expected_registering = static_cast<std::uint32_t>(ConsumerState::kRegistering);
  if (!consumer.state.compare_exchange_strong(
          expected_registering,
          static_cast<std::uint32_t>(ConsumerState::kOnline),
          std::memory_order_seq_cst,
          std::memory_order_seq_cst)) {
    consumer.read_sequence.store(0, std::memory_order_seq_cst);
    std::uint32_t exp_still_registering = static_cast<std::uint32_t>(ConsumerState::kRegistering);
    (void)consumer.state.compare_exchange_strong(
        exp_still_registering,
        static_cast<std::uint32_t>(ConsumerState::kOffline),
        std::memory_order_seq_cst,
        std::memory_order_seq_cst);
    return false;
  }
  const std::uint64_t generation = header_->membership_generation.fetch_add(1, std::memory_order_seq_cst) + 1;
  consumer.owner_pid = static_cast<std::uint32_t>(getpid());
  consumer.owner_start_epoch = static_cast<std::uint32_t>(generation);
  header_->membership_change_count.fetch_add(1, std::memory_order_seq_cst);
  header_->consumer_online_events.fetch_add(1, std::memory_order_seq_cst);
  return true;
}

bool RingLayoutView::UnregisterConsumer(std::uint32_t consumer_index) {
  if (header_ == nullptr || consumers_ == nullptr || consumer_index >= header_->consumer_capacity) {
    return false;
  }
  auto& consumer = consumers_[consumer_index];
  const std::uint32_t previous_state = consumer.state.exchange(
      static_cast<std::uint32_t>(ConsumerState::kOffline),
      std::memory_order_seq_cst);
  if (previous_state != static_cast<std::uint32_t>(ConsumerState::kOnline) &&
      previous_state != static_cast<std::uint32_t>(ConsumerState::kRegistering)) {
    return false;
  }
  consumer.owner_pid = 0;
  consumer.owner_start_epoch = 0;
  // 清除游标，避免后续 `RegisterConsumer` 在 `state=kOnline` 与 `read_sequence=初值` 之间暴露旧游标：
  // 并发 `TryReadNextForConsumer` 若读到「已上线 + 上一世代游标」，会与已复用的槽内序号永久不一致。
  consumer.read_sequence.store(0, std::memory_order_seq_cst);
  header_->membership_generation.fetch_add(1, std::memory_order_seq_cst);
  header_->membership_change_count.fetch_add(1, std::memory_order_seq_cst);
  header_->consumer_offline_events.fetch_add(1, std::memory_order_seq_cst);
  (void)ReclaimCommittedSlots();
  return true;
}

bool RingLayoutView::AddConsumerOnline(std::uint32_t consumer_index) {
  if (header_ == nullptr || consumers_ == nullptr || consumer_index >= header_->consumer_capacity) {
    return false;
  }
  // New consumers start from the current ring head and do not replay historical backlog.
  const std::uint64_t initial_read_sequence = header_->next_sequence.load(std::memory_order_seq_cst);
  return RegisterConsumer(consumer_index, initial_read_sequence == 0 ? 1 : initial_read_sequence);
}

bool RingLayoutView::RemoveConsumerOnline(std::uint32_t consumer_index) {
  return UnregisterConsumer(consumer_index);
}

bool RingLayoutView::RecordProducerBlockDuration(std::uint64_t blocked_ns) {
  if (header_ == nullptr) {
    return false;
  }
  header_->producer_block_total_ns.fetch_add(blocked_ns, std::memory_order_seq_cst);
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

std::optional<std::uint32_t> RingLayoutView::TryAcquireConsumerOnlineSlot() {
  if (header_ == nullptr || consumers_ == nullptr) {
    return std::nullopt;
  }
  const std::uint32_t cap = header_->consumer_capacity;
  if (cap == 0) {
    return std::nullopt;
  }
  constexpr int kSlotClaimRounds = 16;
  for (int round = 0; round < kSlotClaimRounds; ++round) {
    for (std::uint32_t i = 0; i < cap; ++i) {
      if (AddConsumerOnline(i)) {
        return i;
      }
    }
    std::this_thread::yield();
  }
  return std::nullopt;
}

std::uint32_t RingLayoutView::TakeNextProducerPayloadSlotIndex() {
  if (header_ == nullptr) {
    return 0;
  }
  const std::uint32_t slot_count = header_->slot_count;
  if (slot_count == 0) {
    return 0;
  }
  const std::uint64_t ticket =
      header_->producer_payload_slot_ticket.fetch_add(1, std::memory_order_seq_cst);
  return static_cast<std::uint32_t>(ticket % static_cast<std::uint64_t>(slot_count));
}

bool RingLayoutView::PublishCommittedPayload(
    const std::string& channel,
    ByteBuffer payload,
    MessageEnvelope* out_envelope,
    RingHealthMetrics* out_before_metrics,
    RingHealthMetrics* out_after_metrics) {
  if (out_envelope == nullptr) {
    return false;
  }
  auto* header = Header();
  if (header == nullptr || header->slot_count == 0) {
    return false;
  }
  if (out_before_metrics != nullptr) {
    *out_before_metrics = ObserveHealthMetrics();
  }

  const std::size_t payload_capacity = static_cast<std::size_t>(header->payload_region_bytes);
  if (payload.empty() || payload.size() > payload_capacity) {
    return false;
  }

  const std::uint32_t slot_count = header->slot_count;
  const std::size_t per_slot_capacity =
      payload_capacity / static_cast<std::size_t>(std::max<std::uint32_t>(slot_count, 1U));
  if (per_slot_capacity == 0 || payload.size() > per_slot_capacity) {
    return false;
  }
  RingSlotReservation reservation{};
  std::uint32_t slot_index = 0;
  const auto reserve_begin = Clock::now();
  bool warned_no_available_slot = false;
  while (!ReservePublishSlotRingOrdered(static_cast<std::uint32_t>(payload.size()), &reservation, &slot_index)) {
    if (!warned_no_available_slot) {
      LOG(WARNING) << "publish backpressure: ring-ordered reserve failed, channel=" << channel
                   << " slot_count=" << slot_count << " payload_size=" << payload.size();
      warned_no_available_slot = true;
    }
    (void)ReclaimCommittedSlots();
    std::this_thread::yield();
  }
  const auto reserve_elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - reserve_begin).count();
  if (reserve_elapsed > 0) {
    (void)RecordProducerBlockDuration(static_cast<std::uint64_t>(reserve_elapsed));
  }

  std::memcpy(
      PayloadRegion() + reservation.payload_offset,
      payload.data(),
      payload.size());
  if (!CommitSlot(slot_index)) {
    return false;
  }

  out_envelope->channel = channel;
  out_envelope->sequence = reservation.sequence;
  out_envelope->delivery_id = reservation.sequence;
  out_envelope->payload = std::move(payload);
  if (out_after_metrics != nullptr) {
    *out_after_metrics = ObserveHealthMetrics();
  }
  return true;
}

bool RingLayoutView::AdvanceConsumerCursor(std::uint32_t consumer_index, std::uint64_t next_read_sequence) {
  if (header_ == nullptr || consumers_ == nullptr || next_read_sequence == 0 ||
      consumer_index >= header_->consumer_capacity) {
    return false;
  }
  auto& consumer = consumers_[consumer_index];
  if (consumer.state.load(std::memory_order_seq_cst) !=
      static_cast<std::uint32_t>(ConsumerState::kOnline)) {
    return false;
  }

  std::uint64_t current = consumer.read_sequence.load(std::memory_order_seq_cst);
  while (next_read_sequence >= current) {
    if (consumer.read_sequence.compare_exchange_weak(
            current,
            next_read_sequence,
            std::memory_order_seq_cst,
            std::memory_order_seq_cst)) {
      static std::atomic<std::uint64_t> cursor_advance_counter{0};
      const std::uint64_t value = cursor_advance_counter.fetch_add(1, std::memory_order_seq_cst) + 1;
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
  if (consumer.state.load(std::memory_order_seq_cst) !=
      static_cast<std::uint32_t>(ConsumerState::kOnline)) {
    return std::nullopt;
  }
  return consumer.read_sequence.load(std::memory_order_seq_cst);
}

bool RingLayoutView::ConsumerOwnerMatches(
    std::uint32_t consumer_index,
    std::uint32_t owner_pid,
    std::uint32_t owner_epoch) const {
  if (header_ == nullptr || consumers_ == nullptr || consumer_index >= header_->consumer_capacity) {
    return false;
  }
  const auto& consumer = consumers_[consumer_index];
  if (consumer.state.load(std::memory_order_seq_cst) !=
      static_cast<std::uint32_t>(ConsumerState::kOnline)) {
    return false;
  }
  return consumer.owner_pid == owner_pid && consumer.owner_start_epoch == owner_epoch;
}

bool RingLayoutView::TakeConsumerOfflineIfOwner(
    std::uint32_t consumer_index,
    std::uint32_t owner_pid,
    std::uint32_t owner_epoch) {
  if (!ConsumerOwnerMatches(consumer_index, owner_pid, owner_epoch)) {
    return false;
  }
  return UnregisterConsumer(consumer_index);
}

std::optional<std::uint64_t> RingLayoutView::MinActiveReadSequence() const {
  if (header_ == nullptr || consumers_ == nullptr) {
    return std::nullopt;
  }
  while (true) {
    const std::uint64_t generation_before = header_->membership_generation.load(std::memory_order_seq_cst);
    std::optional<std::uint64_t> min_read_sequence;
    for (std::uint32_t i = 0; i < header_->consumer_capacity; ++i) {
      if (consumers_[i].state.load(std::memory_order_seq_cst) !=
          static_cast<std::uint32_t>(ConsumerState::kOnline)) {
        continue;
      }
      const std::uint64_t candidate = consumers_[i].read_sequence.load(std::memory_order_seq_cst);
      if (!min_read_sequence.has_value() || candidate < *min_read_sequence) {
        min_read_sequence = candidate;
      }
    }
    const std::uint64_t generation_after = header_->membership_generation.load(std::memory_order_seq_cst);
    if (generation_before == generation_after) {
      // When no consumers are online, treat the ring head as the effective min cursor.
      // This allows reclaim to progress and keeps producer publish path from stalling.
      if (!min_read_sequence.has_value()) {
        const std::uint64_t next_sequence = header_->next_sequence.load(std::memory_order_seq_cst);
        return next_sequence == 0 ? 1 : next_sequence;
      }
      return min_read_sequence;
    }
  }
}

std::uint64_t RingLayoutView::ReclaimCommittedSlots() {
  if (header_ == nullptr || slots_ == nullptr) {
    return 0;
  }

  // 必须与 `TryReclaimSlotIfSafe` 共用同一条原子控制字回收路径。
  // 旧实现先 `TryReadCommitted` 再无条件 `ResetSlot`，与 `TryReclaimSlotIfSafe`、生产者并发时存在
  // TOCTOU：槽在两次操作之间可能被回收并再次提交，此时 `ResetSlot` 会抹掉新消息并破坏环形序号协议，
  // 表现为消费者长期 `TryReadNextForConsumer` 失败、槽占满、生产者在发布路径上反复重试预留槽位。
  std::uint64_t reclaimed = 0;
  for (std::uint32_t i = 0; i < header_->slot_count; ++i) {
    if (TryReclaimSlotIfSafe(i)) {
      ++reclaimed;
    }
  }
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
  const std::uint64_t next_sequence = header_->next_sequence.load(std::memory_order_seq_cst);
  if (next_sequence <= *read_sequence) {
    return 0;
  }
  return next_sequence - *read_sequence;
}

std::optional<std::uint64_t> RingLayoutView::MembershipGeneration() const {
  if (header_ == nullptr) {
    return std::nullopt;
  }
  return header_->membership_generation.load(std::memory_order_seq_cst);
}

RingHealthMetrics RingLayoutView::ObserveHealthMetrics() const {
  RingHealthMetrics metrics{};
  if (header_ == nullptr) {
    return metrics;
  }
  metrics.occupancy_slots = header_->committed_slots.load(std::memory_order_seq_cst);
  metrics.slot_capacity = header_->slot_count;
  metrics.producer_block_total_ns = header_->producer_block_total_ns.load(std::memory_order_seq_cst);
  metrics.reclaim_count = header_->reclaim_count.load(std::memory_order_seq_cst);
  metrics.membership_change_count = header_->membership_change_count.load(std::memory_order_seq_cst);
  metrics.consumer_online_events = header_->consumer_online_events.load(std::memory_order_seq_cst);
  metrics.consumer_offline_events = header_->consumer_offline_events.load(std::memory_order_seq_cst);

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
  const std::uint64_t committed_control = slot.state_sequence.load(std::memory_order_seq_cst);
  if (UnpackSlotState(committed_control) != SlotState::kCommitted) {
    return false;
  }

  const std::uint64_t committed_sequence = UnpackSlotSequence(committed_control);
  const std::optional<std::uint64_t> min_read_sequence = MinActiveReadSequence();
  if (!min_read_sequence.has_value() || committed_sequence >= *min_read_sequence) {
    return false;
  }
  if (slot.state_sequence.load(std::memory_order_seq_cst) != committed_control) {
    return false;
  }

  const std::uint64_t empty_control = PackSlotControl(SlotState::kEmpty, 0);
  std::uint64_t expected_control = committed_control;
  if (!slot.state_sequence.compare_exchange_strong(
          expected_control,
          empty_control,
          std::memory_order_seq_cst,
          std::memory_order_seq_cst)) {
    return false;
  }

  const std::uint64_t reclaimed_sequence = committed_sequence;

  header_->reclaim_count.fetch_add(1, std::memory_order_seq_cst);
  header_->committed_slots.fetch_sub(1, std::memory_order_seq_cst);

  std::uint64_t watermark = header_->reclaim_sequence.load(std::memory_order_seq_cst);
  while (reclaimed_sequence > watermark &&
         !header_->reclaim_sequence.compare_exchange_weak(
             watermark,
             reclaimed_sequence,
             std::memory_order_seq_cst,
             std::memory_order_seq_cst)) {
  }
  return true;
}

}  // namespace mould::comm
