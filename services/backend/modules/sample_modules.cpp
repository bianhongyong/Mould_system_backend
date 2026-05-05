#include "module_factory_registry.hpp"

#include <atomic>
#include <chrono>
#include <glog/logging.h>
#include <string>
#include <thread>
#include <vector>

namespace mould::backend {
using mould::comm::ModuleBase;
using mould::comm::MessageEnvelope;
using mould::comm::ModuleFactoryConfig;
using mould::comm::ByteBuffer;

namespace {

struct VerifyStats {
  std::atomic<std::uint64_t> ok_count{0};
  std::atomic<std::uint64_t> decode_fail_count{0};
  std::atomic<std::uint64_t> mismatch_count{0};
  std::atomic<std::uint64_t> too_short_count{0};
  std::atomic<std::int64_t> next_report_at_ms{0};
};

void MaybeLogVerifyStats(VerifyStats* stats, const char* module_name, const char* stage_name) {
  if (stats == nullptr) {
    return;
  }
  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  auto scheduled_ms = stats->next_report_at_ms.load(std::memory_order_relaxed);
  if (now_ms < scheduled_ms) {
    return;
  }
  const auto next_ms = now_ms + 1000;
  if (!stats->next_report_at_ms.compare_exchange_strong(scheduled_ms, next_ms,
                                                        std::memory_order_relaxed)) {
    return;
  }
  LOG(ERROR) << "[" << module_name << "] verify_summary stage=" << stage_name
             << " ok=" << stats->ok_count.load(std::memory_order_relaxed)
             << " decode_fail=" << stats->decode_fail_count.load(std::memory_order_relaxed)
             << " mismatch=" << stats->mismatch_count.load(std::memory_order_relaxed)
             << " too_short=" << stats->too_short_count.load(std::memory_order_relaxed);
}

ByteBuffer EncodeRawPayload(std::uint64_t seq) {
  const std::string text = "msg-" + std::to_string(seq);
  return ByteBuffer(text.begin(), text.end());
}

bool DecodeSeqFromRawPayload(const ByteBuffer& payload, std::uint64_t* seq_out) {
  if (seq_out == nullptr) {
    return false;
  }
  constexpr const char* kPrefix = "msg-";
  constexpr std::size_t kPrefixLen = 4;
  if (payload.size() <= kPrefixLen) {
    return false;
  }
  for (std::size_t i = 0; i < kPrefixLen; ++i) {
    if (payload[i] != static_cast<std::uint8_t>(kPrefix[i])) {
      return false;
    }
  }
  std::uint64_t value = 0;
  for (std::size_t i = kPrefixLen; i < payload.size(); ++i) {
    const std::uint8_t ch = payload[i];
    if (ch < static_cast<std::uint8_t>('0') || ch > static_cast<std::uint8_t>('9')) {
      return false;
    }
    value = value * 10 + static_cast<std::uint64_t>(ch - static_cast<std::uint8_t>('0'));
  }
  *seq_out = value;
  return true;
}

void VerifyRawPayloadOrLogError(const ByteBuffer& payload, const char* module_name, VerifyStats* stats) {
  std::uint64_t seq = 0;
  if (!DecodeSeqFromRawPayload(payload, &seq)) {
    if (stats != nullptr) {
      stats->decode_fail_count.fetch_add(1, std::memory_order_relaxed);
    }
    LOG(ERROR) << "[" << module_name << "] decode failed for raw payload_size=" << payload.size();
    MaybeLogVerifyStats(stats, module_name, "raw");
    return;
  }
  const ByteBuffer expected = EncodeRawPayload(seq);
  if (payload != expected) {
    if (stats != nullptr) {
      stats->mismatch_count.fetch_add(1, std::memory_order_relaxed);
    }
    LOG(ERROR) << "[" << module_name << "] raw payload mismatch for seq=" << seq
               << " expected_size=" << expected.size() << " actual_size=" << payload.size();
    MaybeLogVerifyStats(stats, module_name, "raw");
    return;
  }
  if (stats != nullptr) {
    stats->ok_count.fetch_add(1, std::memory_order_relaxed);
  }
  MaybeLogVerifyStats(stats, module_name, "raw");
}

void VerifyFeaturePayloadOrLogError(const ByteBuffer& payload, const char* module_name,
                                    VerifyStats* stats) {
  if (payload.size() <= 1) {
    if (stats != nullptr) {
      stats->too_short_count.fetch_add(1, std::memory_order_relaxed);
    }
    LOG(ERROR) << "[" << module_name << "] feature payload too short, payload_size=" << payload.size();
    MaybeLogVerifyStats(stats, module_name, "feature");
    return;
  }
  const std::uint8_t marker = payload.back();
  ByteBuffer raw_payload(payload.begin(), payload.end() - 1);
  std::uint64_t seq = 0;
  if (!DecodeSeqFromRawPayload(raw_payload, &seq)) {
    if (stats != nullptr) {
      stats->decode_fail_count.fetch_add(1, std::memory_order_relaxed);
    }
    LOG(ERROR) << "[" << module_name << "] decode failed for feature payload_size=" << payload.size();
    MaybeLogVerifyStats(stats, module_name, "feature");
    return;
  }

  ByteBuffer expected = EncodeRawPayload(seq);
  expected.push_back(marker);
  if (payload != expected) {
    if (stats != nullptr) {
      stats->mismatch_count.fetch_add(1, std::memory_order_relaxed);
    }
    LOG(ERROR) << "[" << module_name << "] feature payload mismatch for seq=" << seq
               << " expected_size=" << expected.size() << " actual_size=" << payload.size();
    MaybeLogVerifyStats(stats, module_name, "feature");
    return;
  }
  if (stats != nullptr) {
    stats->ok_count.fetch_add(1, std::memory_order_relaxed);
  }
  MaybeLogVerifyStats(stats, module_name, "feature");
}

void MaybeLogNoMessageWarning(std::chrono::steady_clock::time_point last_recv_at,
                              std::chrono::steady_clock::time_point* next_report_at,
                              const char* module_name,
                              std::size_t recv_count) {
  if (next_report_at == nullptr) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now < *next_report_at) {
    return;
  }
  if (now - last_recv_at < std::chrono::seconds(1)) {
    return;
  }
  LOG(WARNING) << "[" << module_name << "] no message for >=1s, total_received=" << recv_count;
  *next_report_at = now + std::chrono::seconds(1);
}

VerifyStats g_feature_extract_verify_stats;
VerifyStats g_feature_extract_replica_verify_stats;
VerifyStats g_result_sink_verify_stats;
VerifyStats g_result_sink_replica_verify_stats;

}  // namespace

class FrameSourceModule final : public ModuleBase {
 public:
  explicit FrameSourceModule(const ModuleFactoryConfig& config)
      : ModuleBase(config.module_name, config.runtime_context) {}
  ~FrameSourceModule() override {
    producer_stop_.store(true, std::memory_order_release);
    if (producer_thread_.joinable()) {
      producer_thread_.join();
    }
  }

 private:
  bool DoInit() override { 
    bool expected = false;
    if (!producer_started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      return false;
    }
    producer_thread_ = std::thread([this]() {
      auto next_publish_at = std::chrono::steady_clock::now();
      while (!producer_stop_.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_publish_at) {
          std::this_thread::sleep_until(next_publish_at);
          continue;
        }
        next_publish_at = now + kPublishInterval;
        const auto seq = publish_seq_.fetch_add(1, std::memory_order_relaxed);
        ByteBuffer payload = EncodeRawPayload(seq);
        (void)Publish("frame.raw", std::move(payload));
      }
    });
    return true; 
  }
  bool SetupSubscriptions() override { return true; }
  void OnRunIteration() override {
  }

  // 常驻单线程生产，默认约 320 msg/s。
  static constexpr auto kPublishInterval = std::chrono::microseconds(7);
  std::atomic<bool> producer_started_{false};
  std::atomic<bool> producer_stop_{false};
  std::thread producer_thread_;
  std::atomic<std::uint64_t> publish_seq_{0};
};

class FeatureExtractModule final : public ModuleBase {
 public:
  explicit FeatureExtractModule(const ModuleFactoryConfig& config)
      : ModuleBase(config.module_name, config.runtime_context) {}

 private:
  bool DoInit() override { return true; }
  void OnRunIteration() override {
    MaybeLogNoMessageWarning(last_recv_at_, &next_no_msg_report_at_, "FeatureExtractModule",
                             received_count_);
  }
  bool SetupSubscriptions() override {
    return SubscribeOneChannel("frame.raw", [this](const MessageEnvelope& message) {
      last_recv_at_ = std::chrono::steady_clock::now();
      ++received_count_;
      VerifyRawPayloadOrLogError(message.payload, "FeatureExtractModule",
                                 &g_feature_extract_verify_stats);
      ByteBuffer out = message.payload;
      out.push_back(static_cast<std::uint8_t>('!'));
      // if (!Publish("feature.vec", std::move(out))) {
      //   LOG(WARNING) << "[FeatureExtractModule] publish failed channel=feature.vec";
      // }
    });
  }

  std::size_t received_count_ = 0;
  std::chrono::steady_clock::time_point last_recv_at_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point next_no_msg_report_at_ = std::chrono::steady_clock::now();
};

class FeatureExtractReplicaModule final : public ModuleBase {
 public:
  explicit FeatureExtractReplicaModule(const ModuleFactoryConfig& config)
      : ModuleBase(config.module_name, config.runtime_context) {}

 private:
  bool DoInit() override { return true; }
  void OnRunIteration() override {
    MaybeLogNoMessageWarning(last_recv_at_, &next_no_msg_report_at_, "FeatureExtractReplicaModule",
                             received_count_);
  }
  bool SetupSubscriptions() override {
    return SubscribeOneChannel("frame.raw", [this](const MessageEnvelope& message) {
      last_recv_at_ = std::chrono::steady_clock::now();
      ++received_count_;
      VerifyRawPayloadOrLogError(message.payload, "FeatureExtractReplicaModule",
                                 &g_feature_extract_replica_verify_stats);
      LOG(INFO) << "[FeatureExtractReplicaModule] recv channel=frame.raw payload_size="
                << message.payload.size();
      ByteBuffer out = message.payload;
      out.push_back(static_cast<std::uint8_t>('#'));
      LOG(INFO) << "[FeatureExtractReplicaModule] publish channel=feature.vec.replica payload_size="
                << out.size();
      if (!Publish("feature.vec.replica", std::move(out))) {
        //LOG(WARNING) << "[FeatureExtractReplicaModule] publish failed channel=feature.vec.replica";
      }
    });
  }

  std::size_t received_count_ = 0;
  std::chrono::steady_clock::time_point last_recv_at_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point next_no_msg_report_at_ = std::chrono::steady_clock::now();
};

class ResultSinkModule final : public ModuleBase {
 public:
  explicit ResultSinkModule(const ModuleFactoryConfig& config)
      : ModuleBase(config.module_name, config.runtime_context) {}

 private:
  bool DoInit() override { return true; }
  void OnRunIteration() override {
    MaybeLogNoMessageWarning(last_recv_at_, &next_no_msg_report_at_, "ResultSinkModule",
                             received_count_);
  }
  bool SetupSubscriptions() override {
    return SubscribeOneChannel("feature.vec", [this](const MessageEnvelope& message) {
      last_recv_at_ = std::chrono::steady_clock::now();
      VerifyFeaturePayloadOrLogError(message.payload, "ResultSinkModule", &g_result_sink_verify_stats);
      ++received_count_;
      last_payload_size_ = message.payload.size();
      LOG(INFO) << "[ResultSinkModule] recv channel=feature.vec payload_size=" << last_payload_size_
                << " total_received=" << received_count_;
    });
  }

  std::size_t received_count_ = 0;
  std::size_t last_payload_size_ = 0;
  std::chrono::steady_clock::time_point last_recv_at_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point next_no_msg_report_at_ = std::chrono::steady_clock::now();
};

class ResultSinkReplicaModule final : public ModuleBase {
 public:
  explicit ResultSinkReplicaModule(const ModuleFactoryConfig& config)
      : ModuleBase(config.module_name, config.runtime_context) {}

 private:
  bool DoInit() override { return true; }
  void OnRunIteration() override {
    MaybeLogNoMessageWarning(last_recv_at_, &next_no_msg_report_at_, "ResultSinkReplicaModule",
                             received_count_);
  }
  bool SetupSubscriptions() override {
    return SubscribeOneChannel("feature.vec.replica", [this](const MessageEnvelope& message) {
      last_recv_at_ = std::chrono::steady_clock::now();
      VerifyFeaturePayloadOrLogError(message.payload, "ResultSinkReplicaModule",
                                     &g_result_sink_replica_verify_stats);
      ++received_count_;
      last_payload_size_ = message.payload.size();
      LOG(INFO) << "[ResultSinkReplicaModule] recv channel=feature.vec.replica payload_size="
                << last_payload_size_ << " total_received=" << received_count_;
    });
  }

  std::size_t received_count_ = 0;
  std::size_t last_payload_size_ = 0;
  std::chrono::steady_clock::time_point last_recv_at_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point next_no_msg_report_at_ = std::chrono::steady_clock::now();
};

REGISTER_MOULD_MODULE_AS("FrameSourceModule", FrameSourceModule)
REGISTER_MOULD_MODULE_AS("FeatureExtractModule", FeatureExtractModule)
REGISTER_MOULD_MODULE_AS("FeatureExtractReplicaModule", FeatureExtractReplicaModule)
REGISTER_MOULD_MODULE_AS("ResultSinkModule", ResultSinkModule)
REGISTER_MOULD_MODULE_AS("ResultSinkReplicaModule", ResultSinkReplicaModule)

}  // namespace mould::backend
