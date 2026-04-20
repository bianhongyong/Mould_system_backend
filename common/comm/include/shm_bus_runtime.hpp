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

/// Linux SHM pub/sub data plane: publish/subscribe **attach** to segments created by
/// `ShmBusControlPlane::ProvisionChannelTopology` (or equivalent). This class does not create POSIX
/// shared-memory objects. All channel segments are attached during `SetChannelTopology` /
/// `SetChannelTopologyFromModuleConfigs`; `RegisterPublisher` / `Subscribe` / `Publish` only use
/// channels already present in that topology (no lazy attach on first use).
///
/// Subscriptions register notification `eventfd`s only; **one** background pump thread must be
/// started with `StartUnifiedSubscriberPump()` after all `Subscribe` calls (typically one module
/// process). The pump uses a single `epoll` instance for every subscribed channel.
///
/// Taking consumers offline after remote process failure is a control-plane concern; use
/// `ShmBusControlPlane::{ReadConsumerOwnerIdentity,TakeConsumerOfflineIfOwner,...}` from the supervisor.
///
/// Threading: all APIs except the unified subscriber pump thread are assumed to run on one
/// application thread. `runtime_mutex_` serializes **mutations and lookups** of `runtime_by_channel_`
/// (lazy attach, topology setup, pump discovery). Each `ChannelRuntime` is immutable after emplace:
/// `segment` and `ring` are stable handles into fixed shared-memory layout; they are not the locus of
/// publish/subscribe data races. Cross-thread and cross-process correctness for slots, sequences,
/// and consumer membership is enforced inside `RingLayoutView` / ring header atomics (lower layer),
/// not by mutating `ChannelRuntime` fields during publish.
class ShmBusRuntime : public IPubSubBus {
 public:
  explicit ShmBusRuntime(MiddlewareConfig config = {});
  ~ShmBusRuntime() override;

  bool RegisterPublisher(const std::string& module_name, const std::string& channel) override;
  bool Subscribe(
      const std::string& module_name,
      const std::string& channel,
      MessageHandler handler) override;
  bool Publish(const std::string& module_name, const std::string& channel, ByteBuffer payload) override;
  const ReliabilityMetrics& Metrics() const;

  /// Starts a single worker thread that `epoll_wait`s all subscriber notification fds and dispatches
  /// ring messages. Call once after every intended `Subscribe` has succeeded. Returns `true` when
  /// there are zero subscribers (no-op). Returns `false` if a pump thread is already running or
  /// epoll setup fails. After a successful start, further `Subscribe` calls fail until the runtime
  /// is torn down / subscribers are stopped.
  bool StartUnifiedSubscriberPump();
  /// Attach-only: POSIX object and ring must already exist (e.g. after `ShmBusControlPlane::ProvisionChannelTopology`).
  bool SetChannelTopology(mould::config::ChannelTopologyIndex topology);
  bool SetChannelTopologyFromModuleConfigs(
      const std::vector<std::pair<std::string, std::string>>& module_config_files,
      std::string* out_error);
  static std::string BuildDedupKey(const MessageEnvelope& message);

  /// Optional hook after fork(2) in a child process when the bus object existed in the parent.
  /// Starts reactor-local resources only; safe to call once before Subscribe in the child.
  void AfterForkInChildProcess();

 private:
  struct SubscriberEntry {
    std::string channel;
    std::string module_name;
    MessageHandler handler;
    std::uint32_t consumer_index = 0;
    int event_fd = -1;
    std::atomic<bool> stop{false};
    ReliabilityMetrics* metrics = nullptr;
    std::size_t dedup_capacity = 0;
    std::deque<std::string> dedup_window;
    std::unordered_set<std::string> dedup_keys;
  };

  struct UnifiedEpollSlot {
    std::shared_ptr<SubscriberEntry> subscriber;
  };

  /// Per logical channel: attach-only SHM segment plus a view into the ring layout. Filled once when
  /// the channel is mapped; thereafter `segment`/`ring` are not reassigned—concurrency on payload and
  /// consumers is entirely the ring's shared-memory protocol.
  struct ChannelRuntime {
    ShmSegment segment;
    RingLayoutView ring;
  };

  void UnifiedSubscriberPumpLoop();
  static bool TryDispatchOneRingMessage(
      RingLayoutView& ring,
      const std::shared_ptr<SubscriberEntry>& subscriber,
      ConsumerAcker* acker);
  /// Attaches SHM for `channel` which must exist in `topology_index_`. Called only while building
  /// the runtime table during `SetChannelTopology`.
  bool EnsureChannelMappedAttachLocked(const std::string& channel);
  bool ChannelMappedLocked(const std::string& channel) const;
  void StopAllSubscribers();

  /// Protects `runtime_by_channel_` and attach/mapping helpers (including pump re-lookup). Does not
  /// substitute for ring-level synchronization; see `ChannelRuntime` comment.
  std::mutex runtime_mutex_;
  std::thread unified_subscriber_thread_;
  std::atomic<bool> unified_pump_stop_{false};
  int unified_epoll_fd_ = -1;
  std::vector<std::unique_ptr<UnifiedEpollSlot>> unified_epoll_slots_;
  MiddlewareConfig config_;
  ReliabilityMetrics metrics_;
  std::unordered_map<std::string, std::vector<std::shared_ptr<SubscriberEntry>>> subscribers_by_channel_;
  std::unordered_map<std::string, ChannelRuntime> runtime_by_channel_;
  mould::config::ChannelTopologyIndex topology_index_;
  bool topology_initialized_ = false;
  /// After `SetChannelTopology`, lazy channel mapping uses attach-only with this allow-list.
  std::unordered_set<std::string> allowed_shm_channel_keys_;
  bool registry_frozen_ = false;
};

}  // namespace mould::comm
