#include "shm_pubsub_bus.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <glog/logging.h>
#include <sstream>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

namespace mould::comm {

namespace {

constexpr std::uint32_t kDefaultSlotCount = 256;
constexpr std::size_t kEpollEventCapacity = 1;

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

void DrainEventFdSignal(int fd) {
  if (fd < 0) {
    return;
  }
  while (true) {
    std::uint64_t pending_signals = 0;
    const ssize_t read_bytes = read(fd, &pending_signals, sizeof(pending_signals));
    if (read_bytes == static_cast<ssize_t>(sizeof(pending_signals))) {
      return;
    }
    if (read_bytes < 0 && errno == EINTR) {
      continue;
    }
    return;
  }
}

}  // namespace

ShmPubSubBus::ShmPubSubBus(MiddlewareConfig config) : config_(std::move(config)) {
  if (!config_.IsValid()) {
    config_ = MiddlewareConfig{};
  }
}

ShmPubSubBus::~ShmPubSubBus() {
  StopAllSubscribers();
}

bool ShmPubSubBus::RegisterPublisher(const std::string& module_name, const std::string& channel) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!EnsureChannelMappingLocked(channel)) {
    return false;
  }
  auto [iter, inserted] = publishers_by_channel_.try_emplace(channel, module_name);
  if (!inserted && iter->second != module_name) {
    return false;
  }
  return true;
}

bool ShmPubSubBus::Subscribe(
    const std::string& module_name,
    const std::string& channel,
    MessageHandler handler) {
  if (!handler) {
    return false;
  }

  std::shared_ptr<SubscriberEntry> subscriber;
  ChannelRuntime* runtime = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!EnsureChannelMappingLocked(channel)) {
      return false;
    }
    auto runtime_iter = runtime_by_channel_.find(channel);
    if (runtime_iter == runtime_by_channel_.end()) {
      return false;
    }
    runtime = &runtime_iter->second;
    const auto consumer_index = AllocateConsumerIndexLocked(runtime);
    if (!consumer_index.has_value()) {
      return false;
    }
    const int inherited_fd = runtime->ring.LoadConsumerNotificationFd(*consumer_index).value_or(-1);
    if (inherited_fd < 0) {
      return false;
    }

    subscriber = std::make_shared<SubscriberEntry>();
    subscriber->channel = channel;
    subscriber->module_name = module_name;
    subscriber->handler = std::move(handler);
    subscriber->consumer_index = *consumer_index;
    subscriber->event_fd = inherited_fd;
    subscriber->metrics = &metrics_;
    subscriber->dedup_capacity = std::max<std::size_t>(config_.queue_depth, 1U);
    subscribers_by_channel_[channel].push_back(subscriber);
  }

  subscriber->reactor = std::thread(&ShmPubSubBus::SubscriberReactorLoop, subscriber, runtime);
  return true;
}

bool ShmPubSubBus::Publish(const std::string& module_name, const std::string& channel, ByteBuffer payload) {
  std::vector<int> notify_fds;
  MessageEnvelope envelope;
  RingHealthMetrics before_metrics{};
  RingHealthMetrics after_metrics{};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto publisher_iter = publishers_by_channel_.find(channel);
    if (publisher_iter == publishers_by_channel_.end() || publisher_iter->second != module_name) {
      return false;
    }
    auto runtime_iter = runtime_by_channel_.find(channel);
    if (runtime_iter == runtime_by_channel_.end()) {
      return false;
    }
    if (!PublishToRingLocked(
            &runtime_iter->second,
            channel,
            std::move(payload),
            &envelope,
            &before_metrics,
            &after_metrics)) {
      return false;
    }
    auto* consumers = runtime_iter->second.ring.Consumers();
    auto* header = runtime_iter->second.ring.Header();
    if (consumers != nullptr && header != nullptr) {
      notify_fds.reserve(header->notification_capacity);
      for (std::uint32_t i = 0; i < header->notification_capacity; ++i) {
        const auto state = consumers[i].state.load(std::memory_order_acquire);
        if (state != static_cast<std::uint32_t>(ConsumerState::kOnline)) {
          continue;
        }
        const auto fd = runtime_iter->second.ring.LoadConsumerNotificationFd(i);
        if (!fd.has_value() || *fd < 0) {
          continue;
        }
        notify_fds.push_back(*fd);
      }
    }
  }

  const std::uint64_t signal_value = 1;
  for (const int fd : notify_fds) {
    (void)NotifyEventFd(fd, signal_value);
  }

  (void)before_metrics;
  (void)after_metrics;
  return true;
}

void ShmPubSubBus::SubscriberReactorLoop(
    const std::shared_ptr<SubscriberEntry>& subscriber,
    ChannelRuntime* runtime) {
  if (!subscriber || !runtime || subscriber->event_fd < 0) {
    return;
  }

  ConsumerAcker acker(subscriber->metrics);
  const int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0) {
    return;
  }
  epoll_event register_event{};
  register_event.events = EPOLLIN;
  register_event.data.fd = subscriber->event_fd;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, subscriber->event_fd, &register_event) != 0) {
    close(epoll_fd);
    return;
  }

  epoll_event events[kEpollEventCapacity];
  while (!subscriber->stop.load(std::memory_order_acquire)) {
    const int ready = epoll_wait(epoll_fd, events, static_cast<int>(kEpollEventCapacity), 50);
    if (ready <= 0) {
      continue;
    }

    DrainEventFdSignal(subscriber->event_fd);

    while (!subscriber->stop.load(std::memory_order_acquire)) {
      const auto next_read = runtime->ring.LoadConsumerCursor(subscriber->consumer_index);
      if (!next_read.has_value() || *next_read == 0) {
        break;
      }
      const auto slot_count = runtime->ring.Header()->slot_count;
      if (slot_count == 0) {
        break;
      }
      const std::uint32_t slot_index =
          static_cast<std::uint32_t>((*next_read - 1U) % static_cast<std::uint64_t>(slot_count));
      RingSlotReservation reservation{};
      if (!runtime->ring.TryReadCommitted(slot_index, &reservation) || reservation.sequence < *next_read) {
        break;
      }

      MessageEnvelope message;
      message.channel = subscriber->channel;
      message.publisher_module = "shm";
      message.delivery_id = reservation.sequence;
      message.sequence = reservation.sequence;
      message.payload.resize(reservation.payload_size);
      std::memcpy(
          message.payload.data(),
          runtime->ring.PayloadRegion() + reservation.payload_offset,
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

      try {
        subscriber->handler(message);
        acker.Ack(message.delivery_id, message.maybe_duplicate, false);
      } catch (...) {
        acker.Ack(message.delivery_id, message.maybe_duplicate, true);
      }
      (void)runtime->ring.CompleteConsumerSlot(
          subscriber->consumer_index,
          slot_index,
          reservation.sequence + 1U);
    }
  }
  close(epoll_fd);
}

std::optional<std::uint32_t> ShmPubSubBus::AllocateConsumerIndexLocked(ChannelRuntime* runtime) {
  if (runtime == nullptr || runtime->consumer_slots.empty()) {
    return std::nullopt;
  }
  for (std::uint32_t i = 0; i < runtime->consumer_slots.size(); ++i) {
    if (!runtime->consumer_slots[i] && runtime->ring.AddConsumerOnline(i)) {
      runtime->consumer_slots[i] = true;
      return i;
    }
  }
  return std::nullopt;
}

bool ShmPubSubBus::PublishToRingLocked(
    ChannelRuntime* runtime,
    const std::string& channel,
    ByteBuffer payload,
    MessageEnvelope* out_envelope,
    RingHealthMetrics* out_before_metrics,
    RingHealthMetrics* out_after_metrics) {
  if (runtime == nullptr || out_envelope == nullptr) {
    return false;
  }
  auto* header = runtime->ring.Header();
  if (header == nullptr || header->slot_count == 0) {
    return false;
  }
  if (out_before_metrics != nullptr) {
    *out_before_metrics = runtime->ring.ObserveHealthMetrics();
  }

  const std::size_t payload_capacity = static_cast<std::size_t>(header->payload_region_bytes);
  if (payload.empty() || payload.size() > payload_capacity) {
    return false;
  }

  const std::uint32_t slot_count = header->slot_count;
  const std::size_t per_slot_capacity =
      payload_capacity / static_cast<std::size_t>(std::max<std::uint32_t>(slot_count, 1U));
  if (per_slot_capacity == 0 || payload.size() > per_slot_capacity) {
    return false;
  }
  const std::uint32_t slot_index = runtime->next_slot_index % slot_count;
  const std::uint64_t payload_offset =
      static_cast<std::uint64_t>(slot_index) *
      static_cast<std::uint64_t>(per_slot_capacity);
  RingSlotReservation reservation{};
  const auto reserve_begin = Clock::now();
  bool warned_no_available_slot = false;
  while (!runtime->ring.ReserveSlot(
      slot_index,
      static_cast<std::uint32_t>(payload.size()),
      payload_offset,
      &reservation)) {
    if (!warned_no_available_slot) {
      LOG(WARNING) << "publish backpressure: no available slot, channel=" << channel
                   << " slot_index=" << slot_index << " slot_count=" << slot_count
                   << " payload_size=" << payload.size();
      warned_no_available_slot = true;
    }
    std::this_thread::yield();
  }
  const auto reserve_elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - reserve_begin).count();
  if (reserve_elapsed > 0) {
    runtime->ring.RecordProducerBlockDuration(static_cast<std::uint64_t>(reserve_elapsed));
  }

  std::memcpy(
      runtime->ring.PayloadRegion() + reservation.payload_offset,
      payload.data(),
      payload.size());
  if (!runtime->ring.CommitSlot(slot_index)) {
    return false;
  }
  runtime->next_slot_index = (slot_index + 1U) % slot_count;

  out_envelope->channel = channel;
  out_envelope->sequence = reservation.sequence;
  out_envelope->delivery_id = reservation.sequence;
  out_envelope->payload = std::move(payload);
  if (out_after_metrics != nullptr) {
    *out_after_metrics = runtime->ring.ObserveHealthMetrics();
  }
  return true;
}

std::string ShmPubSubBus::BuildDedupKey(const MessageEnvelope& message) {
  if (message.channel.empty() || message.sequence == 0) {
    return {};
  }
  std::ostringstream oss;
  oss << message.channel << "#" << message.sequence;
  return oss.str();
}

void ShmPubSubBus::StopAllSubscribers() {
  std::vector<std::shared_ptr<SubscriberEntry>> subscribers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [channel, entries] : subscribers_by_channel_) {
      (void)channel;
      for (const auto& entry : entries) {
        subscribers.push_back(entry);
      }
    }
    subscribers_by_channel_.clear();
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

  for (const auto& subscriber : subscribers) {
    if (subscriber && subscriber->reactor.joinable()) {
      subscriber->reactor.join();
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> channels_to_unlink;
  channels_to_unlink.reserve(runtime_by_channel_.size());
  for (auto& [channel, runtime] : runtime_by_channel_) {
    channels_to_unlink.push_back(channel);
    (void)channel;
    for (std::uint32_t i = 0; i < runtime.consumer_slots.size(); ++i) {
      const auto fd = runtime.ring.LoadConsumerNotificationFd(i);
      if (fd.has_value() && *fd >= 0) {
        close(*fd);
        runtime.ring.SetConsumerNotificationFd(i, -1);
      }
    }
  }
  runtime_by_channel_.clear();
  for (const auto& channel : channels_to_unlink) {
    const std::string shm_name = BuildDeterministicShmName(channel);
    (void)shm_unlink(shm_name.c_str());
  }
}

bool ShmPubSubBus::EnsureChannelMappingLocked(const std::string& channel) {
  if (!topology_initialized_) {
    return false;
  }
  if (runtime_by_channel_.find(channel) != runtime_by_channel_.end()) {
    return true;
  }

  const auto topology_iter = topology_index_.find(channel);
  const config::ChannelTopologyEntry* entry =
      topology_iter == topology_index_.end() ? nullptr : &topology_iter->second;
  const std::size_t queue_depth = config::ComputeQueueDepthForChannel(entry, config_.queue_depth);

  ShmSegmentLayout layout;
  const std::uint32_t consumer_capacity = entry == nullptr
      ? 1U
      : static_cast<std::uint32_t>(std::max<std::size_t>(entry->consumer_count, 1U));
  const std::size_t ring_layout_bytes = ComputeRingLayoutSizeBytes(kDefaultSlotCount, consumer_capacity);
  layout.payload_capacity = ring_layout_bytes + queue_depth * kDefaultSlotCount;
  auto mapped = ShmSegment::CreateOrAttach(channel, layout);
  if (!mapped.has_value()) {
    return false;
  }

  auto* ring_base = static_cast<std::uint8_t*>(mapped->BaseAddress()) + ShmSegmentHeaderSizeBytes();
  const std::size_t ring_span = mapped->SizeBytes() - ShmSegmentHeaderSizeBytes();
  auto ring = RingLayoutView::Attach(ring_base, ring_span);
  if (!ring.has_value()) {
    ring = RingLayoutView::Initialize(
        ring_base,
        ring_span,
        kDefaultSlotCount,
        consumer_capacity,
        static_cast<std::uint64_t>(queue_depth * kDefaultSlotCount));
  }
  if (!ring.has_value()) {
    return false;
  }

  ChannelRuntime runtime;
  runtime.segment = std::move(*mapped);
  runtime.ring = *ring;
  runtime.next_slot_index = 0;
  runtime.consumer_slots.assign(consumer_capacity, false);
  for (std::uint32_t i = 0; i < consumer_capacity; ++i) {
    int notify_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (notify_fd < 0 || !runtime.ring.SetConsumerNotificationFd(i, notify_fd)) {
      if (notify_fd >= 0) {
        close(notify_fd);
      }
      return false;
    }
  }
  runtime_by_channel_.emplace(channel, std::move(runtime));
  return true;
}

const ReliabilityMetrics& ShmPubSubBus::Metrics() const {
  return metrics_;
}

bool ShmPubSubBus::SetChannelTopology(config::ChannelTopologyIndex topology) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!runtime_by_channel_.empty()) {
    return false;
  }
  topology_index_ = std::move(topology);
  topology_initialized_ = true;
  for (const auto& [channel, entry] : topology_index_) {
    (void)entry;
    if (!EnsureChannelMappingLocked(channel)) {
      topology_initialized_ = false;
      return false;
    }
  }
  return true;
}

bool ShmPubSubBus::SetChannelTopologyFromModuleConfigs(
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

}  // namespace mould::comm
