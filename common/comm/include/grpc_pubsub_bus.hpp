#pragma once

#include "interfaces.hpp"

#include <absl/status/statusor.h>

#include <string>

namespace mould::comm {

/// Placeholder cross-node bus; not part of the Linux SHM data plane / control plane split.
class GrpcPubSubBus final : public IPubSubBus {
 public:
  bool RegisterPublisher(const std::string& module_name, const std::string& channel) override;
  bool Subscribe(
      const std::string& module_name,
      const std::string& channel,
      MessageHandler handler) override;
  bool Publish(const std::string& module_name, const std::string& channel, ByteBuffer payload) override;
  absl::StatusOr<std::uint64_t> PublishWithStatus(
      const std::string& module_name,
      const std::string& channel,
      ByteBuffer payload) override;
};

}  // namespace mould::comm
