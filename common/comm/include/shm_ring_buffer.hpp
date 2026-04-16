#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace mould::comm {

constexpr std::uint32_t kRingLayoutMagic = 0x52494e47;  // "RING"
constexpr std::uint16_t kRingLayoutVersion = 2;

enum class SlotState : std::uint32_t {
  kEmpty = 0,
  kReserved = 1,
  kCommitted = 2,
};

enum class ConsumerState : std::uint32_t {
  kOffline = 0,
  kOnline = 1,
};

struct RingHeader {
  std::uint32_t magic = kRingLayoutMagic;
  std::uint16_t version = kRingLayoutVersion;
  std::uint16_t reserved = 0;
  std::uint32_t slot_count = 0;
  std::uint32_t consumer_capacity = 0;
  std::uint32_t notification_capacity = 0;
  std::uint32_t notification_reserved = 0;
  std::uint64_t payload_region_bytes = 0;
  std::atomic<std::uint64_t> next_sequence = 1;
  std::atomic<std::uint64_t> reclaim_sequence = 1;
  std::atomic<std::uint64_t> membership_generation = 1;
  std::atomic<std::uint64_t> committed_slots = 0;
  std::atomic<std::uint64_t> reclaim_count = 0;
  std::atomic<std::uint64_t> membership_change_count = 0;
  std::atomic<std::uint64_t> consumer_online_events = 0;
  std::atomic<std::uint64_t> consumer_offline_events = 0;
  std::atomic<std::uint64_t> producer_block_total_ns = 0;
};

struct ConsumerCursor {
  std::atomic<std::uint64_t> read_sequence = 0;
  std::atomic<std::uint32_t> state = static_cast<std::uint32_t>(ConsumerState::kOffline);
  std::uint32_t reserved = 0;
};

struct NotificationMappingEntry {
  std::int32_t event_fd = -1;
  std::uint32_t reserved = 0;
};

struct SlotMeta {
  std::atomic<std::uint32_t> state = static_cast<std::uint32_t>(SlotState::kEmpty);
  std::uint32_t payload_size = 0;
  std::uint64_t sequence = 0;
  std::uint64_t payload_offset = 0;
};

struct RingSlotReservation {
  std::uint64_t sequence = 0;
  std::uint64_t payload_offset = 0;
  std::uint32_t payload_size = 0;
};

struct RingHealthMetrics {
  std::uint64_t occupancy_slots = 0;
  std::uint64_t slot_capacity = 0;
  std::uint64_t producer_block_total_ns = 0;
  std::uint64_t max_consumer_lag = 0;
  std::uint64_t reclaim_count = 0;
  std::uint64_t membership_change_count = 0;
  std::uint64_t consumer_online_events = 0;
  std::uint64_t consumer_offline_events = 0;
};

class RingLayoutView {
 public:
  static std::optional<RingLayoutView> Initialize(
      void* base_address,
      std::size_t total_size,
      std::uint32_t slot_count,
      std::uint32_t consumer_capacity,
      std::uint64_t payload_region_bytes);

  static std::optional<RingLayoutView> Attach(
      void* base_address,
      std::size_t total_size,
      std::uint16_t expected_version = kRingLayoutVersion);

  RingLayoutView() = default;

  RingHeader* Header() const;
  ConsumerCursor* Consumers() const;
  NotificationMappingEntry* NotificationMappings() const;
  SlotMeta* Slots() const;
  std::uint8_t* PayloadRegion() const;

  std::size_t TotalSizeBytes() const;
  std::size_t LayoutSizeBytes() const;

  bool ReserveSlot(std::uint32_t slot_index, std::uint32_t payload_size, std::uint64_t payload_offset, RingSlotReservation* out);
  bool CommitSlot(std::uint32_t slot_index);
  bool TryReadCommitted(std::uint32_t slot_index, RingSlotReservation* out) const;
  bool ResetSlot(std::uint32_t slot_index);
  bool RegisterConsumer(std::uint32_t consumer_index, std::uint64_t initial_read_sequence = 1);
  bool UnregisterConsumer(std::uint32_t consumer_index);
  bool AddConsumerOnline(std::uint32_t consumer_index);
  bool RemoveConsumerOnline(std::uint32_t consumer_index);
  bool AdvanceConsumerCursor(std::uint32_t consumer_index, std::uint64_t next_read_sequence);
  bool CompleteConsumerSlot(
      std::uint32_t consumer_index,
      std::uint32_t slot_index,
      std::uint64_t next_read_sequence);
  std::optional<std::uint64_t> LoadConsumerCursor(std::uint32_t consumer_index) const;
  std::optional<std::uint64_t> MinActiveReadSequence() const;
  std::uint64_t ReclaimCommittedSlots();
  std::optional<std::uint64_t> ConsumerLag(std::uint32_t consumer_index) const;
  std::optional<std::uint64_t> MembershipGeneration() const;
  RingHealthMetrics ObserveHealthMetrics() const;
  bool RecordProducerBlockDuration(std::uint64_t blocked_ns);
  bool SetConsumerNotificationFd(std::uint32_t consumer_index, std::int32_t event_fd);
  std::optional<std::int32_t> LoadConsumerNotificationFd(std::uint32_t consumer_index) const;

 private:
  RingLayoutView(
      RingHeader* header,
      ConsumerCursor* consumers,
      NotificationMappingEntry* notification_mappings,
      SlotMeta* slots,
      std::uint8_t* payload_region,
      std::size_t total_size,
      std::size_t layout_size);

  bool IsSlotIndexValid(std::uint32_t slot_index) const;
  bool TryReclaimSlotIfSafe(std::uint32_t slot_index);

  RingHeader* header_ = nullptr;
  ConsumerCursor* consumers_ = nullptr;
  NotificationMappingEntry* notification_mappings_ = nullptr;
  SlotMeta* slots_ = nullptr;
  std::uint8_t* payload_region_ = nullptr;
  std::size_t total_size_ = 0;
  std::size_t layout_size_ = 0;
};

std::size_t ComputeRingLayoutSizeBytes(std::uint32_t slot_count, std::uint32_t consumer_capacity);

}  // namespace mould::comm
