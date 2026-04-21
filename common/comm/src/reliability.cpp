#include "reliability.hpp"

#include <thread>

namespace mould::comm {

bool MiddlewareConfig::IsValid() const {
  return queue_depth > 0 && ttl.count() > 0 && retry_policy.initial_backoff.count() > 0 &&
      retry_policy.backoff_multiplier >= 1.0 && backlog_alarm_threshold <= queue_depth &&
      default_consumer_slots_per_channel >= 1 && default_consumer_slots_per_channel <= 1024 &&
      shm_slot_count >= 1;
}

ReliablePublisher::ReliablePublisher(const MiddlewareConfig& config, ReliabilityMetrics* metrics)
    : config_(config), metrics_(metrics) {}

bool ReliablePublisher::PublishWithRetry(const std::function<bool()>& publish_once) const {
  if (!publish_once || !config_.IsValid()) {
    return false;
  }

  std::chrono::milliseconds wait_time = config_.retry_policy.initial_backoff;
  for (std::uint32_t attempt = 0; attempt <= config_.retry_policy.max_retries; ++attempt) {
    if (publish_once()) {
      return true;
    }

    if (metrics_) {
      metrics_->retry_attempts.fetch_add(1, std::memory_order_relaxed);
    }

    if (attempt == config_.retry_policy.max_retries) {
      break;
    }
    std::this_thread::sleep_for(wait_time);
    wait_time = std::chrono::milliseconds(static_cast<std::int64_t>(
        static_cast<double>(wait_time.count()) * config_.retry_policy.backoff_multiplier));
  }

  if (metrics_) {
    metrics_->publish_failures.fetch_add(1, std::memory_order_relaxed);
  }
  return false;
}

ConsumerAcker::ConsumerAcker(ReliabilityMetrics* metrics) : metrics_(metrics) {}

bool ConsumerAcker::Ack(std::uint64_t delivery_id, bool is_duplicate, bool timeout) {
  if (delivery_id == 0) {
    return false;
  }

  if (!metrics_) {
    return true;
  }

  if (is_duplicate) {
    metrics_->delivery_duplicates.fetch_add(1, std::memory_order_relaxed);
  }
  if (timeout) {
    metrics_->consumer_ack_timeout.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  metrics_->consumer_ack_success.fetch_add(1, std::memory_order_relaxed);
  return true;
}

}  // namespace mould::comm
