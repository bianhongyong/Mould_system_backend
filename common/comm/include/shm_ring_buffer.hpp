#pragma once

#include "interfaces.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace mould::comm {

constexpr std::uint32_t kRingLayoutMagic = 0x52494e47;  // "RING"
constexpr std::uint16_t kRingLayoutVersion = 8;

enum class SlotState : std::uint32_t {
  kEmpty = 0,
  kReserved = 1,
  kCommitted = 2,
};

enum class ConsumerState : std::uint32_t {
  kOffline = 0,
  kOnline = 1,
  /// 仅用于 `RegisterConsumer` 内部：已独占槽位并写入 `read_sequence`，尚未发布为 `kOnline`。
  /// 其它路径必须把该状态视为「未就绪」，避免在弱内存序下把旧游标与 `kOnline` 配对。
  kRegistering = 2,
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
  /// Assigns payload shards across `slot_count` regions; use `ticket % slot_count` before reserve.
  std::atomic<std::uint64_t> producer_payload_slot_ticket = 0;
};

struct ConsumerCursor {
  std::atomic<std::uint64_t> read_sequence = 0;
  std::atomic<std::uint32_t> state = static_cast<std::uint32_t>(ConsumerState::kOffline);
  /// Process that registered this slot (Linux PID). 0 means unset / cleared.
  std::uint32_t owner_pid = 0;
  /// Monotonic-ish generation captured from `RingHeader::membership_generation` at registration;
  /// pairs with `owner_pid` to disambiguate PID reuse.
  std::uint32_t owner_start_epoch = 0;
};

struct NotificationMappingEntry {
  std::int32_t event_fd = -1;
  std::uint32_t reserved = 0;
};

struct SlotMeta {
  // Atomic control word encoding {state, sequence} for lock-free consistency checks.
  std::atomic<std::uint64_t> state_sequence = 0;
  std::uint32_t payload_size = 0;
  /// Compete mode: CAS target. 0 = unclaimed, 1 = already claimed by another consumer.
  /// Reset to 0 when the slot transitions kCommitted -> kEmpty via reclaim.
  /// Sits in natural padding after payload_size; total size remains 24 bytes.
  std::atomic<std::uint32_t> claimed{0};
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

/// Non-owning view into a versioned ring layout in **process-shared** memory. Publication and
/// consumer membership rely on atomics and CAS in `RingHeader` / tables here, not on outer runtime
/// structures holding per-channel mutable publish state.
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

  /// 环形发布预留：下一条将发布消息的全局序号为 S 时，固定在物理槽 `(S - 1) mod slot_count`，
  /// `payload_offset` 为该槽在 payload 区的分片起点（与 `PublishCommittedPayload` 一致）。
  /// 与 `TryReadNextForConsumer` 的 O(1) 寻址约定相同。
  /// 单次尝试：任一步 CAS 失败即返回 false，重试/退避由调用方决定。
  bool ReservePublishSlotRingOrdered(
      std::uint32_t payload_size,
      RingSlotReservation* out,
      std::uint32_t* out_slot_index);
  bool CommitSlot(std::uint32_t slot_index);
  bool TryReadCommitted(std::uint32_t slot_index, RingSlotReservation* out) const;
  /// Compete-mode claim: CAS `claimed` 0→1. Caller must hold cursor at this slot's sequence
  /// (i.e. before `CompleteConsumerSlot`) to prevent concurrent reclaim.
  bool TryClaimSlot(std::uint32_t slot_index);
  /// 按消费者 `read_sequence` 在槽 `(read_sequence - 1) mod slot_count` 上 O(1) 读取已提交消息（环形协议）。
  /// 成功时写入 `out` 与 `out_slot_index`；随后应 `CompleteConsumerSlot(consumer_index, *out_slot_index, out->sequence + 1)`。
  bool TryReadNextForConsumer(
      std::uint32_t consumer_index,
      RingSlotReservation* out,
      std::uint32_t* out_slot_index) const;
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
  bool ConsumerOwnerMatches(std::uint32_t consumer_index, std::uint32_t owner_pid, std::uint32_t owner_epoch) const;
  /// Supervisor/control-plane hook: offline a slot if it still matches the given owner identity.
  bool TakeConsumerOfflineIfOwner(std::uint32_t consumer_index, std::uint32_t owner_pid, std::uint32_t owner_epoch);
  std::optional<std::uint64_t> MinActiveReadSequence() const;
  std::uint64_t ReclaimCommittedSlots();
  std::optional<std::uint64_t> ConsumerLag(std::uint32_t consumer_index) const;
  std::optional<std::uint64_t> MembershipGeneration() const;
  RingHealthMetrics ObserveHealthMetrics() const;
  bool RecordProducerBlockDuration(std::uint64_t blocked_ns);
  bool SetConsumerNotificationFd(std::uint32_t consumer_index, std::int32_t event_fd);
  std::optional<std::int32_t> LoadConsumerNotificationFd(std::uint32_t consumer_index) const;

  /// Scans `[0, consumer_capacity)` and CAS-es a free consumer slot online in shared memory.
  std::optional<std::uint32_t> TryAcquireConsumerOnlineSlot();
  /// 轮转 ticket（与环形序号—槽映射无关）；若发布走 `ReservePublishSlotRingOrdered` / `PublishCommittedPayload`，不应再用于选槽。
  std::uint32_t TakeNextProducerPayloadSlotIndex();
  /// Full publish pipeline (reserve/write/commit) using ring-managed slot assignment.
  bool PublishCommittedPayload(
      const std::string& channel,
      ByteBuffer payload,
      MessageEnvelope* out_envelope,
      RingHealthMetrics* out_before_metrics,
      RingHealthMetrics* out_after_metrics,
      std::string* out_error = nullptr);

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
