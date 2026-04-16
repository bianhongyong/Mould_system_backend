#include "channel_factory.hpp"

#include "shm_pubsub_bus.hpp"

#include <cstdlib>
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
  auto bus = std::make_unique<ShmPubSubBus>();
  const char* encoded_pairs = std::getenv("MOULD_MODULE_CHANNEL_CONFIGS");
  if (encoded_pairs == nullptr) {
    return nullptr;
  }

  const auto module_config_files = ParseModuleConfigPairs(encoded_pairs);
  if (module_config_files.empty()) {
    return nullptr;
  }
  std::string error;
  if (!bus->SetChannelTopologyFromModuleConfigs(module_config_files, &error)) {
    return nullptr;
  }
  return bus;
}

std::shared_ptr<IPubSubBus> ChannelFactory::CreateShared(BusKind kind) {
  return std::shared_ptr<IPubSubBus>(Create(kind).release());
}

}  // namespace mould::comm
