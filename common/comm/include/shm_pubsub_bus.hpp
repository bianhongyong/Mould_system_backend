#pragma once

#include "interfaces.hpp"
#include "reliability.hpp"
#include "shm_ring_buffer.hpp"
#include "shm_segment.hpp"
#include "channel_topology_config.hpp"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mould::comm {

class ShmPubSubBus final : public IPubSubBus {
 public:
  explicit ShmPubSubBus(MiddlewareConfig config = {});
  ~ShmPubSubBus() override;

  bool RegisterPublisher(const std::string& module_name, const std::string& channel) override;
  bool Subscribe(
      const std::string& module_name,
      const std::string& channel,
      MessageHandler handler) override;
  bool Publish(const std::string& module_name, const std::string& channel, ByteBuffer payload) override;
  const ReliabilityMetrics& Metrics() const;
  bool SetChannelTopology(mould::config::ChannelTopologyIndex topology);
  bool SetChannelTopologyFromModuleConfigs(
      const std::vector<std::pair<std::string, std::string>>& module_config_files,
      std::string* out_error);
  static std::string BuildDedupKey(const MessageEnvelope& message);

 private:
  struct SubscriberEntry {
    std::string channel;
    std::string module_name;
    MessageHandler handler;
    std::uint32_t consumer_index = 0;
    int event_fd = -1;
    std::atomic<bool> stop{false};
    std::thread reactor;
    ReliabilityMetrics* metrics = nullptr;
    std::size_t dedup_capacity = 0;
    std::deque<std::string> dedup_window;
    std::unordered_set<std::string> dedup_keys;
  };

  struct ChannelRuntime {
    ShmSegment segment;
    RingLayoutView ring;
    std::uint32_t next_slot_index = 0;
    std::vector<bool> consumer_slots;
  };

  static void SubscriberReactorLoop(
      const std::shared_ptr<SubscriberEntry>& subscriber,
      ChannelRuntime* runtime);
  bool EnsureChannelMappingLocked(const std::string& channel);
  std::optional<std::uint32_t> AllocateConsumerIndexLocked(ChannelRuntime* runtime);
  bool PublishToRingLocked(
      ChannelRuntime* runtime,
      const std::string& channel,
      ByteBuffer payload,
      MessageEnvelope* out_envelope,
      RingHealthMetrics* out_before_metrics,
      RingHealthMetrics* out_after_metrics);
  void StopAllSubscribers();

  std::mutex mutex_;
  MiddlewareConfig config_;
  ReliabilityMetrics metrics_;
  std::unordered_map<std::string, std::string> publishers_by_channel_;
  std::unordered_map<std::string, std::vector<std::shared_ptr<SubscriberEntry>>> subscribers_by_channel_;
  std::unordered_map<std::string, ChannelRuntime> runtime_by_channel_;
  std::unordered_map<std::string, std::uint64_t> channel_sequence_;
  mould::config::ChannelTopologyIndex topology_index_;
  bool topology_initialized_ = false;
};

class GrpcPubSubBus final : public IPubSubBus {
 public:
  bool RegisterPublisher(const std::string& module_name, const std::string& channel) override;
  bool Subscribe(
      const std::string& module_name,
      const std::string& channel,
      MessageHandler handler) override;
  bool Publish(const std::string& module_name, const std::string& channel, ByteBuffer payload) override;
};

}  // namespace mould::comm
