#pragma once

#include "channel_topology_config.hpp"
#include "reliability.hpp"
#include "shm_ring_buffer.hpp"
#include "shm_segment.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mould::comm {

/// Owns shared-memory **channel creation** (POSIX `shm_open` + ring init) and names eligible for
/// final `shm_unlink`. `ShmBusRuntime` attaches only; it does not create segments.
///
/// Supervisor-facing consumer lifecycle hooks attach transiently to POSIX segments (same layout
/// as `ShmBusRuntime`) and mutate ring membership. Per-consumer `eventfd` values are created when
/// the ring layout is first initialized (`RingLayoutView::Initialize`), not by `ShmBusRuntime`.
class ShmBusControlPlane {
 public:
  ShmBusControlPlane() = default;
  ShmBusControlPlane(const ShmBusControlPlane&) = delete;
  ShmBusControlPlane& operator=(const ShmBusControlPlane&) = delete;

  /// Create-or-attach every channel in `topology`, initialize ring layout when missing, then freeze
  /// the canonical POSIX key registry (same policy as historical `ShmBusRuntime::ProvisionChannelTopology`).
  bool ProvisionChannelTopology(
      const mould::config::ChannelTopologyIndex& topology,
      const MiddlewareConfig& middleware_config);

  bool ProvisionChannelTopologyFromModuleConfigs(
      const std::vector<std::pair<std::string, std::string>>& module_config_files,
      std::string* out_error,
      const MiddlewareConfig& middleware_config = MiddlewareConfig{});

  /// Idempotent: unlink every tracked POSIX name and clear the set. Safe on non-owners (ENOENT).
  void FinalizeUnlinkManagedSegments();

  /// Drop bookkeeping without unlinking (e.g. attach-only child process teardown).
  void ClearManagedSegmentNames();

  /// After `SetChannelTopology` succeeds, disallow attaching unknown logical channel keys
  /// (canonical `producer__channel` strings). Idempotent re-call replaces the allow-list.
  void FreezeAllowedChannelKeys(std::unordered_set<std::string> allowed_shm_channel_keys);

  void SetDefaultConsumerSlotsPerChannel(std::uint32_t n);
  std::uint32_t DefaultConsumerSlotsPerChannel() const;

  /// Attach the channel segment, read `(owner_pid, owner_start_epoch)` for an **online** consumer slot.
  /// `topology_entry` must match the entry used when the segment was provisioned (or `nullptr` for
  /// attach-only single-consumer fallback keyed by `logical_channel`).
  bool ReadConsumerOwnerIdentity(
      const std::string& logical_channel,
      const mould::config::ChannelTopologyEntry* topology_entry,
      std::size_t default_slot_payload_bytes,
      std::uint32_t consumer_index,
      std::uint32_t* out_owner_pid,
      std::uint32_t* out_owner_epoch);

  /// Attach, then `TakeConsumerOfflineIfOwner` when `(owner_pid, owner_epoch)` still matches the slot,
  /// and run one reclaim pass. Does not call `shm_unlink`.
  bool TakeConsumerOfflineIfOwner(
      const std::string& logical_channel,
      const mould::config::ChannelTopologyEntry* topology_entry,
      std::size_t default_slot_payload_bytes,
      std::uint32_t consumer_index,
      std::uint32_t owner_pid,
      std::uint32_t owner_epoch);

  /// Scan every channel in `topology`, attach each segment, and offline every **online** slot whose
  /// `owner_pid` equals `dead_owner_pid` (epoch read from shared memory). Returns how many slots
  /// were successfully taken offline.
  std::uint32_t TakeAllConsumersOfflineForProcessPid(
      std::uint32_t dead_owner_pid,
      const mould::config::ChannelTopologyIndex& topology,
      std::size_t default_slot_payload_bytes);

  /// Number of times `FinalizeUnlinkManagedSegments` ran (tests / telemetry).
  std::uint64_t FinalizeUnlinkInvocationCount() const { return finalize_unlink_invocations_.load(); }

 private:
  struct AttachedChannelRing {
    ShmSegment segment;
    RingLayoutView ring;
  };

  std::optional<ShmSegment> CreateOrAttachChannel(const std::string& channel, const ShmSegmentLayout& layout);
  std::optional<ShmSegment> AttachChannel(const std::string& channel, const ShmSegmentLayout& layout);
  std::optional<AttachedChannelRing> AttachChannelRingForSupervisor(
      const std::string& logical_channel,
      const mould::config::ChannelTopologyEntry* topology_entry,
      std::size_t default_slot_payload_bytes);

  std::mutex mutex_;
  std::unordered_set<std::string> managed_posix_names_;
  bool registry_frozen_ = false;
  std::unordered_set<std::string> allowed_shm_channel_keys_;
  std::atomic<std::uint64_t> finalize_unlink_invocations_{0};
  std::uint32_t default_consumer_slots_per_channel_ = 10;
  std::uint32_t shm_slot_count_ = 256;
};

}  // namespace mould::comm
