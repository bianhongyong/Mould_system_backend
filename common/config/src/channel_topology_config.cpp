#include "channel_topology_config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace mould::config {
namespace {

std::string Trim(std::string value) {
  const auto begin = std::find_if(value.begin(), value.end(), [](unsigned char c) {
    return !std::isspace(c);
  });
  const auto end = std::find_if(value.rbegin(), value.rend(), [](unsigned char c) {
    return !std::isspace(c);
  }).base();
  if (begin >= end) {
    return "";
  }
  return std::string(begin, end);
}

bool IsAllowedChannelChar(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
      (value >= '0' && value <= '9') || value == '_' || value == '-' || value == '/' ||
      value == '.';
}

bool IsValidChannelName(const std::string& channel) {
  if (channel.empty()) {
    return false;
  }
  for (const char value : channel) {
    if (!IsAllowedChannelChar(value)) {
      return false;
    }
  }
  return true;
}

bool ParseParamToken(
    const std::string& token,
    std::unordered_map<std::string, std::string>* out_params,
    std::string* out_error) {
  const auto equals_pos = token.find('=');
  if (equals_pos == std::string::npos || equals_pos == 0 || equals_pos + 1 >= token.size()) {
    if (out_error != nullptr) {
      *out_error = "invalid parameter token: " + token;
    }
    return false;
  }

  const std::string key = token.substr(0, equals_pos);
  const std::string value = token.substr(equals_pos + 1);
  if (key.empty() || value.empty()) {
    if (out_error != nullptr) {
      *out_error = "empty key or value in parameter token: " + token;
    }
    return false;
  }

  auto [iter, inserted] = out_params->emplace(key, value);
  if (!inserted && iter->second != value) {
    if (out_error != nullptr) {
      *out_error = "conflicting duplicate parameter '" + key + "'";
    }
    return false;
  }
  return true;
}

void MergeModule(
    const ModuleChannelConfig& module_config,
    ChannelTopologyIndex* topology,
    std::string* out_error,
    bool* ok) {
  auto merge_params = [&](const std::string& channel, const std::unordered_map<std::string, std::string>& params) {
    auto& entry = (*topology)[channel];
    entry.channel = channel;
    for (const auto& [key, value] : params) {
      auto [iter, inserted] = entry.params.emplace(key, value);
      if (!inserted && iter->second != value) {
        *ok = false;
        if (out_error != nullptr) {
          *out_error = "parameter conflict for channel '" + channel + "' key '" + key + "'";
        }
        return;
      }
    }
  };

  for (const auto& output : module_config.output_channels) {
    if (!*ok) {
      return;
    }
    merge_params(output.channel, output.params);
    if (!*ok) {
      return;
    }
    auto& entry = (*topology)[output.channel];
    if (std::find(entry.producers.begin(), entry.producers.end(), module_config.module_name) ==
        entry.producers.end()) {
      entry.producers.push_back(module_config.module_name);
    }
  }

  for (const auto& input : module_config.input_channels) {
    if (!*ok) {
      return;
    }
    merge_params(input.channel, input.params);
    if (!*ok) {
      return;
    }
    auto& entry = (*topology)[input.channel];
    if (std::find(entry.consumers.begin(), entry.consumers.end(), module_config.module_name) ==
        entry.consumers.end()) {
      entry.consumers.push_back(module_config.module_name);
      entry.consumer_count = entry.consumers.size();
    }
  }
}

std::size_t ParsePositiveSizeOrZero(const std::string& value) {
  if (value.empty()) {
    return 0;
  }
  for (const char c : value) {
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      return 0;
    }
  }
  return static_cast<std::size_t>(std::stoull(value));
}

}  // namespace

bool ParseModuleChannelConfigFile(
    const std::string& module_name,
    const std::string& config_path,
    ModuleChannelConfig* out_config,
    std::string* out_error) {
  if (out_config == nullptr) {
    if (out_error != nullptr) {
      *out_error = "output config pointer is null";
    }
    return false;
  }
  std::ifstream input(config_path);
  if (!input.is_open()) {
    if (out_error != nullptr) {
      *out_error = "failed to open config file: " + config_path;
    }
    return false;
  }

  ModuleChannelConfig parsed;
  parsed.module_name = module_name;

  std::unordered_set<std::string> seen_output_channels;
  std::string line;
  std::size_t line_no = 0;
  while (std::getline(input, line)) {
    ++line_no;
    std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }

    std::istringstream iss(trimmed);
    std::string role;
    std::string channel;
    iss >> role >> channel;
    if (role.empty() || channel.empty()) {
      if (out_error != nullptr) {
        *out_error = "line " + std::to_string(line_no) + ": expected '<input|output> <channel>'";
      }
      return false;
    }
    if (!role.empty() && role.back() == ':') {
      role.pop_back();
    }
    if (role != "input" && role != "output") {
      if (out_error != nullptr) {
        *out_error = "line " + std::to_string(line_no) + ": role must be input or output";
      }
      return false;
    }
    if (!IsValidChannelName(channel)) {
      if (out_error != nullptr) {
        *out_error = "line " + std::to_string(line_no) + ": invalid channel name '" + channel + "'";
      }
      return false;
    }

    ChannelEndpointConfig endpoint;
    endpoint.channel = channel;
    std::string token;
    while (iss >> token) {
      if (!ParseParamToken(token, &endpoint.params, out_error)) {
        if (out_error != nullptr) {
          *out_error = "line " + std::to_string(line_no) + ": " + *out_error;
        }
        return false;
      }
    }

    if (role == "output") {
      const auto [iter, inserted] = seen_output_channels.insert(channel);
      (void)iter;
      if (!inserted) {
        if (out_error != nullptr) {
          *out_error = "line " + std::to_string(line_no) +
              ": duplicate output channel in one module: " + channel;
        }
        return false;
      }
      parsed.output_channels.push_back(std::move(endpoint));
    } else {
      parsed.input_channels.push_back(std::move(endpoint));
    }
  }

  *out_config = std::move(parsed);
  return true;
}

bool BuildChannelTopologyIndex(
    const std::vector<ModuleChannelConfig>& module_configs,
    ChannelTopologyIndex* out_topology,
    std::string* out_error) {
  if (out_topology == nullptr) {
    if (out_error != nullptr) {
      *out_error = "output topology pointer is null";
    }
    return false;
  }

  ChannelTopologyIndex topology;
  bool ok = true;
  for (const auto& module_config : module_configs) {
    MergeModule(module_config, &topology, out_error, &ok);
    if (!ok) {
      return false;
    }
  }

  for (auto& [channel, entry] : topology) {
    std::sort(entry.producers.begin(), entry.producers.end());
    std::sort(entry.consumers.begin(), entry.consumers.end());
    entry.consumer_count = entry.consumers.size();
    if (entry.producers.size() > 1) {
      if (out_error != nullptr) {
        *out_error = "channel '" + channel + "' has multiple producers";
      }
      return false;
    }
  }

  *out_topology = std::move(topology);
  return true;
}

bool BuildChannelTopologyIndexFromFiles(
    const std::vector<std::pair<std::string, std::string>>& module_config_files,
    ChannelTopologyIndex* out_topology,
    std::string* out_error) {
  std::vector<ModuleChannelConfig> configs;
  configs.reserve(module_config_files.size());
  for (const auto& [module_name, config_path] : module_config_files) {
    ModuleChannelConfig config;
    if (!ParseModuleChannelConfigFile(module_name, config_path, &config, out_error)) {
      return false;
    }
    configs.push_back(std::move(config));
  }
  return BuildChannelTopologyIndex(configs, out_topology, out_error);
}

std::size_t ComputeQueueDepthForChannel(
    const ChannelTopologyEntry* entry,
    const std::size_t default_queue_depth) {
  std::size_t resolved = std::max<std::size_t>(1, default_queue_depth);
  if (entry == nullptr) {
    return resolved;
  }

  const auto queue_depth_iter = entry->params.find("queue_depth");
  if (queue_depth_iter != entry->params.end()) {
    const std::size_t configured = ParsePositiveSizeOrZero(queue_depth_iter->second);
    if (configured > 0) {
      return configured;
    }
  }
  return resolved;
}

}  // namespace mould::config
