#pragma once

#include <cstddef>
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

std::size_t ComputeQueueDepthForChannel(
    const ChannelTopologyEntry* entry,
    std::size_t default_queue_depth);

}  // namespace mould::config
