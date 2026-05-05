#pragma once

#include "interfaces.hpp"

#include <atomic>
#include <cstdint>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace mould::comm {

struct RetryPolicy {
  std::uint32_t max_retries = 3;
  std::chrono::milliseconds initial_backoff{5};
  double backoff_multiplier = 2.0;
};

struct MiddlewareConfig {
  std::size_t slot_payload_bytes = 1024;
  std::chrono::milliseconds ttl{1000};
  RetryPolicy retry_policy;
  std::size_t backlog_alarm_threshold = 512;
  /// Minimum SHM consumer / notification slots per channel (subscriber preemption pool).
  std::uint32_t default_consumer_slots_per_channel = 10;
  /// SHM ring slot count per channel. Default 256, can be overridden by launch plan communication config.
  std::uint32_t shm_slot_count = 256;

  bool IsValid() const;
};

struct ReliabilityMetrics {
  std::atomic<std::uint64_t> retry_attempts{0};
  std::atomic<std::uint64_t> publish_failures{0};
  std::atomic<std::uint64_t> delivery_duplicates{0};
  std::atomic<std::uint64_t> consumer_ack_success{0};
  std::atomic<std::uint64_t> consumer_ack_timeout{0};
  std::atomic<std::uint64_t> codec_unknown_payload_type_errors{0};
  std::atomic<std::uint64_t> codec_mismatch_errors{0};
  std::atomic<std::uint64_t> image_message_oversize_errors{0};
  std::atomic<std::uint64_t> image_checksum_errors{0};
  std::atomic<std::uint64_t> channel_schema_errors{0};
};

class ReliablePublisher {
 public:
  ReliablePublisher(const MiddlewareConfig& config, ReliabilityMetrics* metrics);

  bool PublishWithRetry(const std::function<bool()>& publish_once) const;

 private:
  MiddlewareConfig config_;
  ReliabilityMetrics* metrics_ = nullptr;
};

class ConsumerAcker {
 public:
  explicit ConsumerAcker(ReliabilityMetrics* metrics);

  // Duplicate deliveries are expected under at-least-once semantics.
  // Business modules must deduplicate with stable business keys (for example request_id).
  bool Ack(std::uint64_t delivery_id, bool is_duplicate, bool timeout);

 private:
  ReliabilityMetrics* metrics_ = nullptr;
};

}  // namespace mould::comm
