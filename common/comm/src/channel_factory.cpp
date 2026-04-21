#include "channel_factory.hpp"

#include "reliability.hpp"
#include "shm_bus_control_plane.hpp"
#include "shm_bus_runtime.hpp"

#include <cstdlib>
#include <cstring>
#include <glog/logging.h>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace mould::comm {
namespace {

std::vector<std::pair<std::string, std::string>> ParseModuleConfigPairs(const std::string& encoded) {
  // Format: moduleA=/path/a.txt;moduleB=/path/b.txt
  std::vector<std::pair<std::string, std::string>> pairs;
  std::stringstream ss(encoded);
  std::string item;
  while (std::getline(ss, item, ';')) {
    if (item.empty()) {
      continue;
    }
    const std::size_t equals = item.find('=');
    if (equals == std::string::npos || equals == 0 || equals + 1 >= item.size()) {
      return {};
    }
    pairs.emplace_back(item.substr(0, equals), item.substr(equals + 1));
  }
  return pairs;
}

std::optional<std::uint32_t> ParsePositiveSlotCount(const char* raw) {
  if (raw == nullptr || raw[0] == '\0') {
    return std::nullopt;
  }
  try {
    std::size_t consumed = 0;
    const unsigned long parsed = std::stoul(raw, &consumed, 10);
    if (consumed != std::strlen(raw) || parsed == 0 ||
        parsed > static_cast<unsigned long>(std::numeric_limits<std::uint32_t>::max())) {
      return std::nullopt;
    }
    return static_cast<std::uint32_t>(parsed);
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace

std::unique_ptr<IPubSubBus> ChannelFactory::Create(BusKind kind) {
  (void)kind;  // Current phase always resolves to the single-node SHM bus.
  const char* encoded_pairs = std::getenv("MOULD_MODULE_CHANNEL_CONFIGS");
  if (encoded_pairs == nullptr) {
    LOG(ERROR) << "ChannelFactory::Create failed: MOULD_MODULE_CHANNEL_CONFIGS missing";
    return nullptr;
  }

  const auto module_config_files = ParseModuleConfigPairs(encoded_pairs);
  if (module_config_files.empty()) {
    LOG(ERROR) << "ChannelFactory::Create failed: ParseModuleConfigPairs returned empty, raw="
               << encoded_pairs;
    return nullptr;
  }
  std::string error;
  MiddlewareConfig middleware_config;
  const char* raw_slot_count = std::getenv("MOULD_SHM_SLOT_COUNT");
  if (const auto slot_count = ParsePositiveSlotCount(raw_slot_count); slot_count.has_value()) {
    middleware_config.shm_slot_count = *slot_count;
  } else if (raw_slot_count != nullptr && raw_slot_count[0] != '\0') {
    LOG(WARNING) << "ChannelFactory::Create ignore invalid MOULD_SHM_SLOT_COUNT=" << raw_slot_count
                 << ", fallback to default 256";
  }
  const char* fork_token = std::getenv("MOULD_FORK_INHERITANCE_TOKEN");
  const bool post_fork_child = fork_token != nullptr && fork_token[0] != '\0';
  if (post_fork_child) {
    auto bus = std::make_unique<ShmBusRuntime>(middleware_config);
    if (!bus->SetChannelTopologyFromModuleConfigs(module_config_files, &error)) {
      LOG(ERROR) << "ChannelFactory::Create child path failed: SetChannelTopologyFromModuleConfigs error="
                 << error;
      return nullptr;
    }
    return bus;
  }
  ShmBusControlPlane control_plane;
  if (!control_plane.ProvisionChannelTopologyFromModuleConfigs(
          module_config_files, &error, middleware_config)) {
    LOG(ERROR) << "ChannelFactory::Create parent path failed: ProvisionChannelTopologyFromModuleConfigs error="
               << error;
    return nullptr;
  }
  auto bus = std::make_unique<ShmBusRuntime>(middleware_config);
  if (!bus->SetChannelTopologyFromModuleConfigs(module_config_files, &error)) {
    LOG(ERROR) << "ChannelFactory::Create parent path failed: SetChannelTopologyFromModuleConfigs error="
               << error;
    return nullptr;
  }
  return bus;
}

std::shared_ptr<IPubSubBus> ChannelFactory::CreateShared(BusKind kind) {
  return std::shared_ptr<IPubSubBus>(Create(kind).release());
}

}  // namespace mould::comm
