#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <string>
#include <unordered_map>
#include <vector>

namespace mould::config {

struct ChannelEndpointConfig {
  std::string channel;
  std::unordered_map<std::string, std::string> params;
};

struct ModuleChannelConfig {
  std::string module_name;
  std::vector<ChannelEndpointConfig> input_channels;
  std::vector<ChannelEndpointConfig> output_channels;
};

struct ChannelTopologyEntry {
  std::string channel;
  std::vector<std::string> producers;
  std::vector<std::string> consumers;
  std::size_t consumer_count = 0;
  std::unordered_map<std::string, std::string> params;
};

using ChannelTopologyIndex = std::unordered_map<std::string, ChannelTopologyEntry>;

/// SHM bus per-channel delivery: all subscribers notified vs single-subscriber compete.
enum class ShmBusDeliveryMode {
  kBroadcast,
  kCompete,
};

enum class ChannelPayloadType {
  kUnknown,
  kProtobuf,
  kImage,
  kBinaryBlob,
};

const char* ChannelPayloadTypeName(ChannelPayloadType type);
ChannelPayloadType ResolveChannelPayloadType(const ChannelTopologyEntry& entry);
bool ValidateChannelSchemaGovernance(
    const ChannelTopologyEntry& entry,
    std::string* out_error);
bool ValidateProtoReservedFieldNumbers(
    const std::string& proto_text,
    std::string* out_error);

bool ParseModuleChannelConfigFile(
    const std::string& module_name,
    const std::string& config_path,
    ModuleChannelConfig* out_config,
    std::string* out_error);

bool BuildChannelTopologyIndex(
    const std::vector<ModuleChannelConfig>& module_configs,
    ChannelTopologyIndex* out_topology,
    std::string* out_error);

bool BuildChannelTopologyIndexFromFiles(
    const std::vector<std::pair<std::string, std::string>>& module_config_files,
    ChannelTopologyIndex* out_topology,
    std::string* out_error);

std::size_t ResolveSlotPayloadBytesForChannel(
    const ChannelTopologyEntry* entry,
    std::size_t default_slot_payload_bytes);

std::uint32_t ResolveShmSlotCountForChannel(
    const ChannelTopologyEntry* entry,
    std::uint32_t default_shm_slot_count);

/// Resolves SHM ring `consumer_capacity` / `notification_capacity`: at least
/// `default_consumer_slots_per_channel` (for subscriber slot preemption), or
/// channel-level override `shm_consumer_slots` when configured, and at least the
/// topology-declared consumer module count when non-zero.
std::uint32_t ResolveShmRingConsumerCapacity(
    const ChannelTopologyEntry* entry,
    std::uint32_t default_consumer_slots_per_channel);

/// Reads `delivery_mode` from channel params (`broadcast` default; `compete` for single-notify + cursor sync).
ShmBusDeliveryMode ResolveShmBusDeliveryModeForChannel(const ChannelTopologyEntry* entry);

/// Returns true if `value` is empty, `broadcast`, or `compete` (ASCII case-insensitive).
bool IsValidShmBusDeliveryModeParamValue(const std::string& value);

/// Logical channel key for POSIX shared-memory object names: `primary_module + "__" + channel`,
/// where `primary_module` prefers the first producer when present, otherwise the first consumer.
/// Falls back to `entry.channel` when both lists are empty.
std::string CanonicalShmChannelKey(const ChannelTopologyEntry& entry);

}  // namespace mould::config
