#include "image_frame_channel_helpers.hpp"
#include "module_factory_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <glog/logging.h>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

namespace mould::backend {
using mould::comm::ByteBuffer;
using mould::comm::MessageEnvelope;
using mould::comm::ModuleBase;
using mould::comm::ModuleFactoryConfig;

namespace {

ByteBuffer ToPayload(const std::string& text) {
  return ByteBuffer(text.begin(), text.end());
}

std::string ToText(const ByteBuffer& payload) {
  return std::string(payload.begin(), payload.end());
}

std::string ReadField(const std::string& packet, const std::string& key) {
  const std::string marker = key + "=";
  const std::size_t begin = packet.find(marker);
  if (begin == std::string::npos) {
    return "";
  }
  const std::size_t value_start = begin + marker.size();
  const std::size_t end = packet.find(';', value_start);
  return packet.substr(value_start, end == std::string::npos ? std::string::npos : end - value_start);
}

int ReadFieldInt(const std::string& packet, const std::string& key, int fallback) {
  const std::string value = ReadField(packet, key);
  if (value.empty()) {
    return fallback;
  }
  try {
    return std::stoi(value);
  } catch (...) {
    return fallback;
  }
}

}  // namespace

class SensorFusionModule final : public ModuleBase {
 public:
  explicit SensorFusionModule(const ModuleFactoryConfig& config)
      : ModuleBase(config.module_name, config.runtime_context) {}
  ~SensorFusionModule() override {
    running_.store(false, std::memory_order_release);
    if (producer_.joinable()) {
      producer_.join();
    }
  }

 private:
  bool DoInit() override {
    image_paths_ = image_frame_channel::ListDatasetImagePaths(
        std::filesystem::path(image_frame_channel::kMvtecBottleGoodImageDir));
    if (image_paths_.empty()) {
      LOG(ERROR) << "[SensorFusionModule] no images under dataset dir="
                 << image_frame_channel::kMvtecBottleGoodImageDir
                 << "; ImageFrame publishes will be skipped until files exist";
    } else {
      LOG(INFO) << "[SensorFusionModule] indexed " << image_paths_.size() << " image(s) for ImageFrame publish";
    }
    running_.store(true, std::memory_order_release);
    producer_ = std::thread([this]() {
      auto next_burst_at = std::chrono::steady_clock::now();
      auto next_image_at = std::chrono::steady_clock::now();
      while (running_.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_burst_at) {
          next_burst_at = now + kTelemetryBurstInterval;
          PublishTelemetryBurst();
        }
        if (now >= next_image_at) {
          next_image_at = now + kImagePublishInterval;
          PublishImageFrameFromDataset();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });
    return true;
  }

  bool SetupSubscriptions() override {
    const bool audit_sink_ok =
        SubscribeOneChannel("SensorFusionAuditEvent", [](const MessageEnvelope& /*message*/) {});
    const bool command_ok = SubscribeOneChannel("ActuatorCommand", [this](const MessageEnvelope& message) {
      const std::string packet = ToText(message.payload);
      latest_throttle_ = ReadFieldInt(packet, "throttle", latest_throttle_);
      ++command_observed_count_;
      PublishAudit("command_feedback");
    });
    const bool heartbeat_ok = SubscribeOneChannel("SyncHeartbeat", [this](const MessageEnvelope& message) {
      const std::string packet = ToText(message.payload);
      coordinator_epoch_ = ReadFieldInt(packet, "epoch", coordinator_epoch_);
      ++heartbeat_count_;
    });
    const bool alert_ok = SubscribeOneChannel("DiagnosisAlert", [this](const MessageEnvelope& message) {
      const std::string packet = ToText(message.payload);
      last_alert_level_ = ReadFieldInt(packet, "level", last_alert_level_);
      PublishAudit("diagnosis_feedback");
    });
    return audit_sink_ok && command_ok && heartbeat_ok && alert_ok;
  }

  void OnRunIteration() override {
    ++on_run_iteration_counter_;
    LOG(INFO) << "[SensorFusionModule] on_run_iteration_counter=" << on_run_iteration_counter_;
    if (publish_seq_ % 25 == 0) {
      PublishAudit("fusion_checkpoint");
    }
  }

  void PublishTelemetryBurst() {
    ++publish_seq_;
    const int base_quality = std::clamp(98 - last_alert_level_ * 7, 40, 99);
    const int stability = std::clamp(70 + heartbeat_count_ - last_alert_level_ * 2, 20, 100);
    std::ostringstream telemetry;
    telemetry << "seq=" << publish_seq_ << ";quality=" << base_quality << ";stability=" << stability
              << ";throttle=" << latest_throttle_ << ";epoch=" << coordinator_epoch_ << ";";
    Publish("TelemetryIngress", ToPayload(telemetry.str()));

    std::ostringstream tracks;
    tracks << "seq=" << publish_seq_ << ";objects=" << (6 + publish_seq_ % 5)
           << ";ego_speed=" << (20 + latest_throttle_) << ";risk_hint=" << last_alert_level_ << ";";
    Publish("PerceptionTrack", ToPayload(tracks.str()));
  }

  void PublishImageFrameFromDataset() {
    if (image_paths_.empty()) {
      return;
    }
    const auto seq = image_publish_seq_.fetch_add(1, std::memory_order_relaxed);
    const std::size_t idx = static_cast<std::size_t>(seq) % image_paths_.size();
    const auto& path = image_paths_[idx];
    mould::common::image::ImageFrame frame;
    if (!image_frame_channel::FillImageFrameFromFile(path, static_cast<std::uint64_t>(seq), &frame)) {
      return;
    }
    mould::comm::ByteBuffer wire = image_frame_channel::SerializeImageFrame(frame);
    if (!Publish("ImageFrame", std::move(wire))) {
      LOG(WARNING) << "[SensorFusionModule] Publish ImageFrame failed seq=" << seq;
    }
  }

  void PublishAudit(const std::string& stage) {
    std::ostringstream audit;
    audit << "module=SensorFusionModule;stage=" << stage << ";seq=" << publish_seq_
          << ";cmd_seen=" << command_observed_count_ << ";hb_seen=" << heartbeat_count_ << ";";
    Publish("SensorFusionAuditEvent", ToPayload(audit.str()));
  }

  static constexpr auto kTelemetryBurstInterval = std::chrono::milliseconds(50);
  static constexpr auto kImagePublishInterval = std::chrono::milliseconds(300);

  std::uint64_t on_run_iteration_counter_ = 0;
  std::atomic<bool> running_{false};
  std::thread producer_;
  int publish_seq_ = 0;
  std::atomic<std::uint64_t> image_publish_seq_{0};
  std::vector<std::filesystem::path> image_paths_;
  int latest_throttle_ = 10;
  int coordinator_epoch_ = 0;
  int heartbeat_count_ = 0;
  int command_observed_count_ = 0;
  int last_alert_level_ = 0;
};

REGISTER_MOULD_MODULE_AS("SensorFusionModule", SensorFusionModule)

}  // namespace mould::backend
