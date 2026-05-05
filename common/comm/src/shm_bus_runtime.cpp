#include "shm_bus_runtime.hpp"
#include "grpc_pubsub_bus.hpp"
#include "unified_output_envelope.pb.h"

#include "channel_topology_config.hpp"

#include <absl/status/status.h>
#include <absl/status/statusor.h>

#include <algorithm>
#include <cstring>
#include <cerrno>
#include <cstdint>
#include <mutex>
#include <thread>
#include <glog/logging.h>
#include <sstream>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>

namespace mould::comm {

namespace {

constexpr int kMaxEpollEventsPerWait = 256;

bool NotifyEventFd(int fd, std::uint64_t signal_value) {
  if (fd < 0) {
    return false;
  }
  while (true) {
    const ssize_t written = write(fd, &signal_value, sizeof(signal_value));
    if (written == static_cast<ssize_t>(sizeof(signal_value))) {
      return true;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written < 0 && errno == EAGAIN) {
      return true;
    }
    return false;
  }
}

/// Reads the accumulated `eventfd(2)` counter in one syscall (drains all coalesced publishes for this fd).
bool ReadEventFdNotifyCount(int fd, std::uint64_t* out_count) {
  if (fd < 0 || out_count == nullptr) {
    return false;
  }
  while (true) {
    std::uint64_t pending_signals = 0;
    const ssize_t read_bytes = read(fd, &pending_signals, sizeof(pending_signals));
    if (read_bytes == static_cast<ssize_t>(sizeof(pending_signals))) {
      *out_count = pending_signals;
      return true;
    }
    if (read_bytes < 0 && errno == EINTR) {
      continue;
    }
    *out_count = 0;
    return false;
  }
}

bool RuntimeRegistryAllowsKey(
    const std::unordered_set<std::string>& allowed,
    bool frozen,
    const std::string& channel) {
  return !frozen || allowed.find(channel) != allowed.end();
}

}  // namespace

ShmBusRuntime::ShmBusRuntime(MiddlewareConfig config) : config_(std::move(config)) {
  if (!config_.IsValid()) {
    config_ = MiddlewareConfig{};
  }
}

ShmBusRuntime::~ShmBusRuntime() {
  StopAllSubscribers();
}

bool ShmBusRuntime::RegisterPublisher(const std::string& module_name, const std::string& channel) {
  (void)module_name;
  std::lock_guard<std::mutex> lock(runtime_mutex_);
  return ChannelMappedLocked(channel);
}

bool ShmBusRuntime::Subscribe(
    const std::string& module_name,
    const std::string& channel,
    MessageHandler handler) {
  if (!handler) {
    return false;
  }

  std::shared_ptr<SubscriberEntry> subscriber;
  {
    std::lock_guard<std::mutex> lock(runtime_mutex_);
    if (unified_subscriber_thread_.joinable()) {
      return false;
    }
    if (!ChannelMappedLocked(channel)) {
      return false;
    }
    auto runtime_iter = runtime_by_channel_.find(channel);
    if (runtime_iter == runtime_by_channel_.end()) {
      return false;
    }
    ChannelRuntime* runtime = &runtime_iter->second;
    const auto consumer_index = runtime->ring.TryAcquireConsumerOnlineSlot();
    if (!consumer_index.has_value()) {
      return false;
    }
    const int inherited_fd = runtime->ring.LoadConsumerNotificationFd(*consumer_index).value_or(-1);
    if (inherited_fd < 0) {
      (void)runtime->ring.RemoveConsumerOnline(*consumer_index);
      return false;
    }

    subscriber = std::make_shared<SubscriberEntry>();
    subscriber->channel = channel;
    subscriber->module_name = module_name;
    subscriber->handler = std::move(handler);
    subscriber->consumer_index = *consumer_index;
    subscriber->event_fd = inherited_fd;
    subscriber->metrics = &metrics_;
    subscriber->dedup_capacity = std::max<std::size_t>(config_.slot_payload_bytes, 1U);
    subscribers_by_channel_[channel].push_back(subscriber);
  }

  return true;
}

bool ShmBusRuntime::StartUnifiedSubscriberPump() {
  if (unified_subscriber_thread_.joinable()) {
    return false;
  }

  std::vector<std::shared_ptr<SubscriberEntry>> all;
  for (const auto& [channel_key, entries] : subscribers_by_channel_) {
    (void)channel_key;
    for (const auto& entry : entries) {
      if (entry) {
        all.push_back(entry);
      }
    }
  }
  if (all.empty()) {
    return true;
  }

  unified_epoll_slots_.clear();
  const int epfd = epoll_create1(EPOLL_CLOEXEC);
  if (epfd < 0) {
    return false;
  }

  for (const auto& sub : all) {
    auto slot = std::make_unique<UnifiedEpollSlot>();
    slot->subscriber = sub;
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = slot.get();
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, sub->event_fd, &ev) != 0) {
      (void)close(epfd);
      unified_epoll_slots_.clear();
      return false;
    }
    unified_epoll_slots_.push_back(std::move(slot));
  }

  unified_epoll_fd_ = epfd;
  unified_pump_stop_.store(false, std::memory_order_release);

  unified_subscriber_thread_ = std::thread(&ShmBusRuntime::UnifiedSubscriberPumpLoop, this);
  return true;
}

bool ShmBusRuntime::TryDispatchOneRingMessage(
    RingLayoutView& ring,
    const std::shared_ptr<SubscriberEntry>& subscriber,
    ConsumerAcker* acker,
    mould::config::ShmBusDeliveryMode delivery_mode) {
  if (!subscriber || !acker) {
    VLOG(1) << "TryDispatchOneRingMessage: null subscriber or acker";
    return false;
  }
  if (subscriber->stop.load(std::memory_order_acquire)) {
    VLOG(1) << "TryDispatchOneRingMessage: subscriber stop channel=" << subscriber->channel;
    return false;
  }

  const auto next_read = ring.LoadConsumerCursor(subscriber->consumer_index);
  if (!next_read.has_value() || *next_read == 0) {
    VLOG(1) << "TryDispatchOneRingMessage: no cursor channel=" << subscriber->channel
            << " has_next=" << next_read.has_value()
            << " next_read=" << (next_read.has_value() ? *next_read : 0U);
    return false;
  }
  const auto* header = ring.Header();
  if (header == nullptr) {
    VLOG(1) << "TryDispatchOneRingMessage: null header channel=" << subscriber->channel;
    return false;
  }
  if (header->slot_count == 0) {
    VLOG(1) << "TryDispatchOneRingMessage: slot_count=0 channel=" << subscriber->channel;
    return false;
  }
  RingSlotReservation reservation{};
  std::uint32_t slot_index = 0;
  if (!ring.TryReadNextForConsumer(subscriber->consumer_index, &reservation, &slot_index)) {
    VLOG(1) << "TryDispatchOneRingMessage: TryReadNextForConsumer false (strict no-skip) channel="
            << subscriber->channel << " consumer_index=" << subscriber->consumer_index
            << " next_read=" << *next_read;
    return false;
  }
  if (reservation.sequence < *next_read) {
    VLOG(1) << "TryDispatchOneRingMessage: sequence behind cursor channel=" << subscriber->channel
            << " reservation_sequence=" << reservation.sequence << " next_read=" << *next_read
            << " slot_index=" << slot_index;
    return false;
  }

  MessageEnvelope message;
  message.channel = subscriber->channel;
  message.publisher_module = "shm";
  message.delivery_id = reservation.sequence;
  message.sequence = reservation.sequence;
  message.payload.resize(reservation.payload_size);
  std::memcpy(
      message.payload.data(),
      ring.PayloadRegion() + reservation.payload_offset,
      reservation.payload_size);

  const std::string dedup_key = BuildDedupKey(message);
  const bool duplicate_detected =
      !dedup_key.empty() &&
      subscriber->dedup_keys.find(dedup_key) != subscriber->dedup_keys.end();
  message.maybe_duplicate = duplicate_detected;
  if (!dedup_key.empty() && !duplicate_detected) {
    subscriber->dedup_window.push_back(dedup_key);
    subscriber->dedup_keys.insert(dedup_key);
    while (subscriber->dedup_window.size() > subscriber->dedup_capacity) {
      const std::string& oldest = subscriber->dedup_window.front();
      subscriber->dedup_keys.erase(oldest);
      subscriber->dedup_window.pop_front();
    }
  }

  if (delivery_mode == mould::config::ShmBusDeliveryMode::kCompete) {
    // Compete mode: try to claim the slot. Only the winner invokes the handler.
    const bool claimed = ring.TryClaimSlot(slot_index);
    if (claimed) {
      try {
        subscriber->handler(message);
        acker->Ack(message.delivery_id, message.maybe_duplicate, false);
      } catch (...) {
        acker->Ack(message.delivery_id, message.maybe_duplicate, true);
      }
    } else {
      VLOG(2) << "TryDispatchOneRingMessage: compete slot already claimed channel="
              << subscriber->channel << " sequence=" << reservation.sequence;
    }
  } else {
    // Broadcast mode: every consumer processes the message.
    try {
      subscriber->handler(message);
      acker->Ack(message.delivery_id, message.maybe_duplicate, false);
    } catch (...) {
      acker->Ack(message.delivery_id, message.maybe_duplicate, true);
    }
  }

  // Always advance cursor (compete mode: winner and losers both advance).
  const bool complete_ok = ring.CompleteConsumerSlot(
      subscriber->consumer_index,
      slot_index,
      reservation.sequence + 1U);
  if (!complete_ok) {
    LOG(WARNING) << "TryDispatchOneRingMessage: CompleteConsumerSlot failed channel=" << subscriber->channel
                 << " consumer_index=" << subscriber->consumer_index
                 << " sequence=" << reservation.sequence;
    return false;
  }
  return true;
}

void ShmBusRuntime::UnifiedSubscriberPumpLoop() {
  ConsumerAcker acker(&metrics_);
  const int max_events =
      std::max(1, std::min(kMaxEpollEventsPerWait, static_cast<int>(unified_epoll_slots_.size())));
  std::vector<epoll_event> events(static_cast<std::size_t>(max_events));

  while (!unified_pump_stop_.load(std::memory_order_acquire)) {
    const int epfd = unified_epoll_fd_;
    if (epfd < 0) {
      break;
    }
    const int ready = epoll_wait(epfd, events.data(), max_events, 50);
    if (unified_pump_stop_.load(std::memory_order_acquire)) {
      break;
    }
    if (ready <= 0) {
      continue;
    }
    for (int i = 0; i < ready; ++i) {
      auto* slot = static_cast<UnifiedEpollSlot*>(events[static_cast<std::size_t>(i)].data.ptr);
      if (slot == nullptr || !slot->subscriber) {
        continue;
      }
      const std::shared_ptr<SubscriberEntry> sub = slot->subscriber;
      if (sub->event_fd < 0) {
        continue;
      }
      std::uint64_t notify_budget = 0;
      if (!ReadEventFdNotifyCount(sub->event_fd, &notify_budget)) {
        LOG(WARNING) << "shm_pump: eventfd read failed after EPOLLIN channel=" << sub->channel
                     << " event_fd=" << sub->event_fd;
        continue;
      }
      if (notify_budget == 0) {
        LOG(WARNING) << "shm_pump: eventfd read returned 0 budget after EPOLLIN channel=" << sub->channel
                     << " event_fd=" << sub->event_fd
                     << " (another thread may have read the same fd, or spurious wake)";
        continue;
      }

      mould::config::ShmBusDeliveryMode delivery_mode = mould::config::ShmBusDeliveryMode::kBroadcast;
      RingLayoutView ring;
      {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        const auto runtime_iter = runtime_by_channel_.find(sub->channel);
        if (runtime_iter == runtime_by_channel_.end()) {
          LOG(WARNING) << "shm_pump: channel runtime missing after eventfd budget=" << notify_budget
                       << " channel=" << sub->channel;
          continue;
        }
        ring = runtime_iter->second.ring;
        delivery_mode = runtime_iter->second.delivery_mode;
      }

      VLOG(1) << "shm_pump: eventfd budget channel=" << sub->channel << " event_fd=" << sub->event_fd
              << " notify_budget=" << notify_budget;

      std::uint64_t dispatched = 0;
      for (std::uint64_t n = 0; n < notify_budget; ++n) {
        if (unified_pump_stop_.load(std::memory_order_acquire)) {
          break;
        }
        if (!TryDispatchOneRingMessage(ring, sub, &acker, delivery_mode)) {
          break;
        }
        ++dispatched;
      }
      if (dispatched < notify_budget) {
        LOG(INFO) << "shm_pump: dispatched fewer than eventfd budget channel=" << sub->channel
                  << " event_fd=" << sub->event_fd << " notify_budget=" << notify_budget
                  << " dispatched=" << dispatched
                  << " (eventfd counter already drained; see VLOG TryDispatch* if GLOG_v>=1)";
      }
    }
  }

  if (unified_epoll_fd_ >= 0) {
    (void)close(unified_epoll_fd_);
    unified_epoll_fd_ = -1;
  }
}
bool ShmBusRuntime::Publish(const std::string& module_name, const std::string& channel, ByteBuffer payload) {
  auto result = PublishWithStatus(module_name, channel, std::move(payload));
  if (!result.ok()) {
    LOG(WARNING) << "ShmBusRuntime::Publish failed, module=" << module_name
                 << " channel=" << channel
                 << " reason=" << result.status();
    return false;
  }
  return true;
}

bool ShmBusRuntime::Publish(
    const std::string& module_name,
    const std::string& channel,
    const mould::test::unified::UnifiedOutputEnvelope& envelope) {
  std::string serialized;
  if (!envelope.SerializeToString(&serialized)) {
    LOG(WARNING) << "ShmBusRuntime::Publish proto serialize failed, module=" << module_name
                 << " channel=" << channel;
    return false;
  }
  ByteBuffer payload(serialized.begin(), serialized.end());
  auto result = PublishWithStatus(module_name, channel, std::move(payload));
  if (!result.ok()) {
    LOG(WARNING) << "ShmBusRuntime::Publish failed, module=" << module_name
                 << " channel=" << channel
                 << " reason=" << result.status();
    return false;
  }
  return true;
}


absl::StatusOr<std::uint64_t> ShmBusRuntime::PublishWithStatus(
    const std::string& module_name, const std::string& channel, ByteBuffer payload) {
  RingLayoutView ring;
  {
    std::lock_guard<std::mutex> lock(runtime_mutex_);
    const auto runtime_iter = runtime_by_channel_.find(channel);
    if (runtime_iter == runtime_by_channel_.end()) {
      return absl::NotFoundError("channel runtime not found: " + channel);
    }
    const auto topology_iter = topology_index_.find(channel);
    if (topology_iter == topology_index_.end()) {
      return absl::NotFoundError("channel topology not found: " + channel);
    }
    ring = runtime_iter->second.ring;
  }

  MessageEnvelope envelope;
  RingHealthMetrics before_metrics{};
  RingHealthMetrics after_metrics{};
  std::string publish_error;
  if (!ring.PublishCommittedPayload(
          channel,
          std::move(payload),
          &envelope,
          &before_metrics,
          &after_metrics,
          &publish_error)) {
    const std::string reason =
        publish_error.empty() ? "PublishCommittedPayload failed (unknown)" : publish_error;
    return absl::UnavailableError("publish failed on channel " + channel + ": " + reason);
  }

  envelope.publisher_module = module_name;

  struct OnlineNotify {
    std::uint32_t consumer_index = 0;
    int fd = -1;
  };
  std::vector<OnlineNotify> online;
  auto* consumers = ring.Consumers();
  auto* header = ring.Header();
  if (consumers != nullptr && header != nullptr) {
    online.reserve(header->notification_capacity);
    for (std::uint32_t i = 0; i < header->notification_capacity; ++i) {
      const auto state = consumers[i].state.load(std::memory_order_acquire);
      if (state != static_cast<std::uint32_t>(ConsumerState::kOnline)) {
        continue;
      }
      const auto fd = ring.LoadConsumerNotificationFd(i);
      if (!fd.has_value() || *fd < 0) {
        continue;
      }
      online.push_back(OnlineNotify{i, *fd});
    }
  }

  // Both broadcast and compete modes notify ALL online consumers.
  // In compete mode, consumers race via TryClaimSlot; only the winner processes.
  const std::uint64_t signal_value = 1;
  for (const auto& c : online) {
    (void)NotifyEventFd(c.fd, signal_value);
  }

  (void)before_metrics;
  (void)after_metrics;
  return envelope.sequence;
}

std::string ShmBusRuntime::BuildDedupKey(const MessageEnvelope& message) {
  if (message.channel.empty() || message.sequence == 0) {
    return {};
  }
  std::ostringstream oss;
  oss << message.channel << "#" << message.sequence;
  return oss.str();
}

void ShmBusRuntime::StopAllSubscribers() {
  unified_pump_stop_.store(true, std::memory_order_release);

  std::vector<std::shared_ptr<SubscriberEntry>> subscribers;
  for (const auto& [channel, entries] : subscribers_by_channel_) {
    (void)channel;
    for (const auto& entry : entries) {
      subscribers.push_back(entry);
    }
  }

  for (const auto& subscriber : subscribers) {
    if (!subscriber) {
      continue;
    }
    subscriber->stop.store(true, std::memory_order_release);
    if (subscriber->event_fd >= 0) {
      const std::uint64_t signal = 1;
      (void)NotifyEventFd(subscriber->event_fd, signal);
    }
  }

  if (unified_subscriber_thread_.joinable()) {
    unified_subscriber_thread_.join();
  }

  subscribers_by_channel_.clear();
  unified_epoll_slots_.clear();
  {
    std::lock_guard<std::mutex> lock(runtime_mutex_);
    for (auto& [channel, runtime] : runtime_by_channel_) {
      (void)channel;
      auto* hdr = runtime.ring.Header();
      const std::uint32_t cap = hdr != nullptr ? hdr->consumer_capacity : 0U;
      for (std::uint32_t i = 0; i < cap; ++i) {
        const auto fd = runtime.ring.LoadConsumerNotificationFd(i);
        if (fd.has_value() && *fd >= 0) {
          close(*fd);
          runtime.ring.SetConsumerNotificationFd(i, -1);
        }
      }
    }
    runtime_by_channel_.clear();
  }
}

bool ShmBusRuntime::ChannelMappedLocked(const std::string& channel) const {
  return topology_initialized_ && runtime_by_channel_.find(channel) != runtime_by_channel_.end();
}

bool ShmBusRuntime::EnsureChannelMappedAttachLocked(const std::string& channel) {
  const auto topology_iter = topology_index_.find(channel);
  if (topology_iter == topology_index_.end()) {
    LOG(WARNING) << "shm runtime: channel not in topology; refusing attach channel=" << channel;
    return false;
  }
  if (runtime_by_channel_.find(channel) != runtime_by_channel_.end()) {
    return true;
  }
  const config::ChannelTopologyEntry& entry = topology_iter->second;
  const std::size_t slot_payload_bytes =
      config::ResolveSlotPayloadBytesForChannel(&entry, config_.slot_payload_bytes);
  const std::uint32_t slot_count =
      config::ResolveShmSlotCountForChannel(&entry, config_.shm_slot_count);

  const std::string shm_channel_key = config::CanonicalShmChannelKey(entry);

  ShmSegmentLayout layout;
  const std::uint32_t consumer_capacity =
      config::ResolveShmRingConsumerCapacity(&entry, config_.default_consumer_slots_per_channel);
  const std::size_t ring_layout_bytes = ComputeRingLayoutSizeBytes(slot_count, consumer_capacity);
  layout.payload_capacity = ring_layout_bytes + slot_payload_bytes * slot_count;
  if (!RuntimeRegistryAllowsKey(allowed_shm_channel_keys_, registry_frozen_, shm_channel_key)) {
    LOG(WARNING) << "shm runtime: rejected Attach; registry frozen for key=" << shm_channel_key;
    return false;
  }
  auto mapped = ShmSegment::Attach(shm_channel_key, layout);
  if (!mapped.has_value()) {
    return false;
  }

  auto* ring_base = static_cast<std::uint8_t*>(mapped->BaseAddress()) + ShmSegmentHeaderSizeBytes();
  const std::size_t ring_span = mapped->SizeBytes() - ShmSegmentHeaderSizeBytes();
  auto ring = RingLayoutView::Attach(ring_base, ring_span);
  if (!ring.has_value()) {
    return false;
  }

  ChannelRuntime runtime;
  runtime.segment = std::move(*mapped);
  runtime.ring = *ring;
  runtime.delivery_mode = config::ResolveShmBusDeliveryModeForChannel(&entry);
  runtime_by_channel_.emplace(channel, std::move(runtime));
  return true;
}

const ReliabilityMetrics& ShmBusRuntime::Metrics() const {
  return metrics_;
}

bool ShmBusRuntime::SetChannelTopology(config::ChannelTopologyIndex topology) {
  std::lock_guard<std::mutex> lock(runtime_mutex_);
  if (!runtime_by_channel_.empty()) {
    return false;
  }
  topology_index_ = std::move(topology);
  topology_initialized_ = true;
  registry_frozen_ = false;
  allowed_shm_channel_keys_.clear();
  for (const auto& [channel, entry] : topology_index_) {
    (void)entry;
    if (!EnsureChannelMappedAttachLocked(channel)) {
      topology_initialized_ = false;
      topology_index_.clear();
      runtime_by_channel_.clear();
      allowed_shm_channel_keys_.clear();
      registry_frozen_ = false;
      return false;
    }
  }
  std::unordered_set<std::string> frozen_keys;
  frozen_keys.reserve(topology_index_.size());
  for (const auto& [channel, entry] : topology_index_) {
    (void)channel;
    frozen_keys.insert(config::CanonicalShmChannelKey(entry));
  }
  allowed_shm_channel_keys_ = std::move(frozen_keys);
  registry_frozen_ = true;
  return true;
}

void ShmBusRuntime::AfterForkInChildProcess() {
  // Fork-safety: inherited fds + SHM mapping; call `Subscribe` then `StartUnifiedSubscriberPump` in the child.
  // Reserved for explicit child-side setup when supervisor wires prefork singletons.
}

bool ShmBusRuntime::SetChannelTopologyFromModuleConfigs(
    const std::vector<std::pair<std::string, std::string>>& module_config_files,
    std::string* out_error) {
  config::ChannelTopologyIndex topology;
  if (!config::BuildChannelTopologyIndexFromFiles(module_config_files, &topology, out_error)) {
    return false;
  }
  return SetChannelTopology(std::move(topology));
}

bool GrpcPubSubBus::RegisterPublisher(const std::string& /*module_name*/, const std::string& /*channel*/) {
  return false;
}

bool GrpcPubSubBus::Subscribe(
    const std::string& /*module_name*/,
    const std::string& /*channel*/,
    MessageHandler /*handler*/) {
  return false;
}

bool GrpcPubSubBus::Publish(
    const std::string& /*module_name*/,
    const std::string& /*channel*/,
    ByteBuffer /*payload*/) {
  return false;
}

absl::StatusOr<std::uint64_t> GrpcPubSubBus::PublishWithStatus(
    const std::string& /*module_name*/,
    const std::string& channel,
    ByteBuffer /*payload*/) {
  return absl::UnimplementedError("GrpcPubSubBus::PublishWithStatus is not implemented, channel=" + channel);
}

}  // namespace mould::comm
