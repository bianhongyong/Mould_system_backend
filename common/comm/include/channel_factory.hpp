#pragma once

#include "interfaces.hpp"

#include <memory>

namespace mould::comm {

enum class BusKind {
  kSingleNodeShm,
  kGrpc,
};

class ChannelFactory {
 public:
  static std::unique_ptr<IPubSubBus> Create(BusKind kind = BusKind::kSingleNodeShm);
  static std::shared_ptr<IPubSubBus> CreateShared(BusKind kind = BusKind::kSingleNodeShm);
};

}  // namespace mould::comm
