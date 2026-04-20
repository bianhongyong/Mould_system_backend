#include "channel_factory.hpp"

#include "reliability.hpp"
#include "shm_bus_control_plane.hpp"
#include "shm_bus_runtime.hpp"

#include <cstdlib>
#include <cstring>
#include <memory>
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

}  // namespace

std::unique_ptr<IPubSubBus> ChannelFactory::Create(BusKind kind) {
  (void)kind;  // Current phase always resolves to the single-node SHM bus.
  const char* encoded_pairs = std::getenv("MOULD_MODULE_CHANNEL_CONFIGS");
  if (encoded_pairs == nullptr) {
    return nullptr;
  }

  const auto module_config_files = ParseModuleConfigPairs(encoded_pairs);
  if (module_config_files.empty()) {
    return nullptr;
  }
  std::string error;
  MiddlewareConfig middleware_config;
  const char* fork_token = std::getenv("MOULD_FORK_INHERITANCE_TOKEN");
  const bool post_fork_child = fork_token != nullptr && fork_token[0] != '\0';
  if (post_fork_child) {
    auto bus = std::make_unique<ShmBusRuntime>(middleware_config);
    if (!bus->SetChannelTopologyFromModuleConfigs(module_config_files, &error)) {
      return nullptr;
    }
    return bus;
  }
  ShmBusControlPlane control_plane;
  if (!control_plane.ProvisionChannelTopologyFromModuleConfigs(
          module_config_files, &error, middleware_config)) {
    return nullptr;
  }
  auto bus = std::make_unique<ShmBusRuntime>(middleware_config);
  if (!bus->SetChannelTopologyFromModuleConfigs(module_config_files, &error)) {
    return nullptr;
  }
  return bus;
}

std::shared_ptr<IPubSubBus> ChannelFactory::CreateShared(BusKind kind) {
  return std::shared_ptr<IPubSubBus>(Create(kind).release());
}

}  // namespace mould::comm
