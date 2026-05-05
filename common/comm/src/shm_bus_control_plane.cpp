#include "shm_bus_control_plane.hpp"

#include "channel_topology_config.hpp"
#include "shm_ring_buffer.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <glog/logging.h>
#include <sys/mman.h>

namespace mould::comm {

namespace {

bool RegistryAllowsKey(
    const std::unordered_set<std::string>& allowed,
    bool frozen,
    const std::string& channel) {
  return !frozen || allowed.find(channel) != allowed.end();
}

}  // namespace

void ShmBusControlPlane::SetDefaultConsumerSlotsPerChannel(std::uint32_t n) {
  default_consumer_slots_per_channel_ = std::max<std::uint32_t>(1U, n);
}

std::uint32_t ShmBusControlPlane::DefaultConsumerSlotsPerChannel() const {
  return default_consumer_slots_per_channel_;
}

std::optional<ShmSegment> ShmBusControlPlane::CreateOrAttachChannel(
    const std::string& channel,
    const ShmSegmentLayout& layout) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!RegistryAllowsKey(allowed_shm_channel_keys_, registry_frozen_, channel)) {
      LOG(WARNING) << "shm control plane: rejected CreateOrAttach; registry frozen for key=" << channel;
      return std::nullopt;
    }
  }
  auto segment = ShmSegment::CreateOrAttach(channel, layout);
  if (!segment.has_value()) {
    return std::nullopt;
  }
  if (segment->IsOwner()) {
    std::lock_guard<std::mutex> lock(mutex_);
    managed_posix_names_.insert(segment->Name());
  }
  return segment;
}

std::optional<ShmSegment> ShmBusControlPlane::AttachChannel(
    const std::string& channel,
    const ShmSegmentLayout& layout) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!RegistryAllowsKey(allowed_shm_channel_keys_, registry_frozen_, channel)) {
      LOG(WARNING) << "shm control plane: rejected Attach; registry frozen for key=" << channel;
      return std::nullopt;
    }
  }
  return ShmSegment::Attach(channel, layout);
}

std::optional<ShmBusControlPlane::AttachedChannelRing> ShmBusControlPlane::AttachChannelRingForSupervisor(
    const std::string& logical_channel,
    const mould::config::ChannelTopologyEntry* topology_entry,
    std::size_t default_slot_payload_bytes) {
  const mould::config::ChannelTopologyEntry* entry = topology_entry;
  const std::string shm_channel_key =
      entry != nullptr ? mould::config::CanonicalShmChannelKey(*entry) : logical_channel;
  const std::size_t slot_payload_bytes =
      mould::config::ResolveSlotPayloadBytesForChannel(entry, default_slot_payload_bytes);
  const std::uint32_t consumer_capacity =
      mould::config::ResolveShmRingConsumerCapacity(entry, DefaultConsumerSlotsPerChannel());
  const std::uint32_t slot_count =
      mould::config::ResolveShmSlotCountForChannel(entry, shm_slot_count_);
  ShmSegmentLayout layout{};
  const std::size_t ring_layout_bytes =
      ComputeRingLayoutSizeBytes(slot_count, consumer_capacity);
  layout.payload_capacity = ring_layout_bytes + slot_payload_bytes * slot_count;
  auto mapped = AttachChannel(shm_channel_key, layout);
  if (!mapped.has_value()) {
    return std::nullopt;
  }
  auto* ring_base =
      static_cast<std::uint8_t*>(mapped->BaseAddress()) + ShmSegmentHeaderSizeBytes();
  const std::size_t ring_span = mapped->SizeBytes() - ShmSegmentHeaderSizeBytes();
  auto ring = RingLayoutView::Attach(ring_base, ring_span);
  if (!ring.has_value()) {
    return std::nullopt;
  }
  return AttachedChannelRing{std::move(*mapped), *ring};
}

bool ShmBusControlPlane::ProvisionChannelTopology(
    const mould::config::ChannelTopologyIndex& topology,
    const MiddlewareConfig& middleware_config) {
  MiddlewareConfig cfg = middleware_config;
  if (!cfg.IsValid()) {
    cfg = MiddlewareConfig{};
  }
  SetDefaultConsumerSlotsPerChannel(cfg.default_consumer_slots_per_channel);
  shm_slot_count_ = std::max<std::uint32_t>(1U, cfg.shm_slot_count);

  for (const auto& channel_and_entry : topology) {
    const std::string& channel = channel_and_entry.first;
    const mould::config::ChannelTopologyEntry& entry = channel_and_entry.second;
    const mould::config::ChannelTopologyEntry* entry_ptr = &entry;
    const std::size_t slot_payload_bytes =
        mould::config::ResolveSlotPayloadBytesForChannel(entry_ptr, cfg.slot_payload_bytes);
    const std::string shm_key = mould::config::CanonicalShmChannelKey(entry);
    const std::uint32_t consumer_capacity =
        mould::config::ResolveShmRingConsumerCapacity(entry_ptr, default_consumer_slots_per_channel_);
    ShmSegmentLayout layout{};
    const std::uint32_t slot_count =
        mould::config::ResolveShmSlotCountForChannel(entry_ptr, shm_slot_count_);
    const std::size_t ring_layout_bytes =
        ComputeRingLayoutSizeBytes(slot_count, consumer_capacity);
    layout.payload_capacity = ring_layout_bytes + slot_payload_bytes * slot_count;
    auto mapped = CreateOrAttachChannel(shm_key, layout);
    if (!mapped.has_value()) {
      return false;
    }
    auto* ring_base = static_cast<std::uint8_t*>(mapped->BaseAddress()) + ShmSegmentHeaderSizeBytes();
    const std::size_t ring_span = mapped->SizeBytes() - ShmSegmentHeaderSizeBytes();
    auto ring = RingLayoutView::Attach(ring_base, ring_span);
    if (!ring.has_value()) {
      ring = RingLayoutView::Initialize(
          ring_base,
          ring_span,
          slot_count,
          consumer_capacity,
          static_cast<std::uint64_t>(slot_payload_bytes * slot_count));
    }
    if (!ring.has_value()) {
      return false;
    }
    (void)channel;
  }

  std::unordered_set<std::string> frozen_keys;
  frozen_keys.reserve(topology.size());
  for (const auto& [ch, ent] : topology) {
    (void)ch;
    frozen_keys.insert(mould::config::CanonicalShmChannelKey(ent));
  }
  FreezeAllowedChannelKeys(std::move(frozen_keys));
  return true;
}

bool ShmBusControlPlane::ProvisionChannelTopologyFromModuleConfigs(
    const std::vector<std::pair<std::string, std::string>>& module_config_files,
    std::string* out_error,
    const MiddlewareConfig& middleware_config) {
  mould::config::ChannelTopologyIndex topology;
  if (!mould::config::BuildChannelTopologyIndexFromFiles(module_config_files, &topology, out_error)) {
    return false;
  }
  return ProvisionChannelTopology(topology, middleware_config);
}

void ShmBusControlPlane::FinalizeUnlinkManagedSegments() {
  finalize_unlink_invocations_.fetch_add(1, std::memory_order_relaxed);
  std::unordered_set<std::string> names;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    names.swap(managed_posix_names_);
  }
  LOG(INFO) << "shm control plane: policy final unlink of " << names.size() << " managed POSIX segments";
  for (const std::string& name : names) {
    (void)shm_unlink(name.c_str());
  }
}

void ShmBusControlPlane::ClearManagedSegmentNames() {
  std::lock_guard<std::mutex> lock(mutex_);
  managed_posix_names_.clear();
}

void ShmBusControlPlane::FreezeAllowedChannelKeys(std::unordered_set<std::string> allowed_shm_channel_keys) {
  std::lock_guard<std::mutex> lock(mutex_);
  allowed_shm_channel_keys_ = std::move(allowed_shm_channel_keys);
  registry_frozen_ = true;
}

bool ShmBusControlPlane::ReadConsumerOwnerIdentity(
    const std::string& logical_channel,
    const mould::config::ChannelTopologyEntry* topology_entry,
    std::size_t default_slot_payload_bytes,
    std::uint32_t consumer_index,
    std::uint32_t* out_owner_pid,
    std::uint32_t* out_owner_epoch) {
  if (out_owner_pid == nullptr || out_owner_epoch == nullptr) {
    return false;
  }
  const auto attached = AttachChannelRingForSupervisor(logical_channel, topology_entry, default_slot_payload_bytes);
  if (!attached.has_value()) {
    return false;
  }
  RingHeader* header = attached->ring.Header();
  ConsumerCursor* consumers = attached->ring.Consumers();
  if (header == nullptr || consumers == nullptr || consumer_index >= header->consumer_capacity) {
    return false;
  }
  const ConsumerCursor& slot = consumers[consumer_index];
  if (slot.state.load(std::memory_order_acquire) !=
      static_cast<std::uint32_t>(ConsumerState::kOnline)) {
    return false;
  }
  *out_owner_pid = slot.owner_pid;
  *out_owner_epoch = slot.owner_start_epoch;
  return true;
}

bool ShmBusControlPlane::TakeConsumerOfflineIfOwner(
    const std::string& logical_channel,
    const mould::config::ChannelTopologyEntry* topology_entry,
    std::size_t default_slot_payload_bytes,
    std::uint32_t consumer_index,
    std::uint32_t owner_pid,
    std::uint32_t owner_epoch) {
  auto attached = AttachChannelRingForSupervisor(logical_channel, topology_entry, default_slot_payload_bytes);
  if (!attached.has_value()) {
    return false;
  }
  if (!attached->ring.TakeConsumerOfflineIfOwner(consumer_index, owner_pid, owner_epoch)) {
    return false;
  }
  (void)attached->ring.ReclaimCommittedSlots();
  return true;
}

std::uint32_t ShmBusControlPlane::TakeAllConsumersOfflineForProcessPid(
    std::uint32_t dead_owner_pid,
    const mould::config::ChannelTopologyIndex& topology,
    std::size_t default_slot_payload_bytes) {
  std::uint32_t count = 0;
  for (const auto& channel_and_entry : topology) {
    const std::string& logical_channel = channel_and_entry.first;
    const mould::config::ChannelTopologyEntry& entry = channel_and_entry.second;
    auto attached = AttachChannelRingForSupervisor(logical_channel, &entry, default_slot_payload_bytes);
    if (!attached.has_value()) {
      continue;
    }
    RingHeader* header = attached->ring.Header();
    ConsumerCursor* consumers = attached->ring.Consumers();
    if (header == nullptr || consumers == nullptr) {
      continue;
    }
    for (std::uint32_t i = 0; i < header->consumer_capacity; ++i) {
      const ConsumerCursor& slot = consumers[i];
      if (slot.state.load(std::memory_order_acquire) !=
          static_cast<std::uint32_t>(ConsumerState::kOnline)) {
        continue;
      }
      if (slot.owner_pid != dead_owner_pid) {
        continue;
      }
      const std::uint32_t epoch = slot.owner_start_epoch;
      if (attached->ring.TakeConsumerOfflineIfOwner(i, dead_owner_pid, epoch)) {
        (void)attached->ring.ReclaimCommittedSlots();
        ++count;
      }
    }
  }
  return count;
}

}  // namespace mould::comm
