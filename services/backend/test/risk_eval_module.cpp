#include "image_frame_channel_helpers.hpp"
#include "module_factory_registry.hpp"

#include <algorithm>
#include <cstdint>
#include <glog/logging.h>
#include <sstream>
#include <string>

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

class RiskEvalModule final : public ModuleBase {
 public:
  explicit RiskEvalModule(const ModuleFactoryConfig& config)
      : ModuleBase(config.module_name, config.runtime_context) {}

 private:
  bool DoInit() override { return true; }

  bool SetupSubscriptions() override {
    const bool audit_sink_ok =
        SubscribeOneChannel("RiskEvalAuditEvent", [](const MessageEnvelope& /*message*/) {});
    const bool telemetry_ok = SubscribeOneChannel("TelemetryIngress", [this](const MessageEnvelope& message) {
      const std::string packet = ToText(message.payload);
      latest_quality_ = ReadFieldInt(packet, "quality", latest_quality_);
      latest_stability_ = ReadFieldInt(packet, "stability", latest_stability_);
      RecomputeAndPublish("telemetry");
    });
    const bool track_ok = SubscribeOneChannel("PerceptionTrack", [this](const MessageEnvelope& message) {
      const std::string packet = ToText(message.payload);
      latest_object_count_ = ReadFieldInt(packet, "objects", latest_object_count_);
      latest_ego_speed_ = ReadFieldInt(packet, "ego_speed", latest_ego_speed_);
      RecomputeAndPublish("track");
    });
    const bool heartbeat_ok = SubscribeOneChannel("SyncHeartbeat", [this](const MessageEnvelope& message) {
      const std::string packet = ToText(message.payload);
      coordinator_epoch_ = ReadFieldInt(packet, "epoch", coordinator_epoch_);
      RecomputeAndPublish("heartbeat");
    });
    const bool image_ok = SubscribeOneChannel("ImageFrame", [this](const MessageEnvelope& message) {
      mould::common::image::ImageFrame frame;
      if (!frame.ParseFromArray(message.payload.data(), static_cast<int>(message.payload.size()))) {
        return;
      }
      (void)image_frame_channel::SaveReceivedImageFrameFromMessage(frame, "RiskEvalModule");
      latest_image_quality_ = 90;
      latest_image_stability_ = 70;
      latest_image_size_bytes_ = frame.image_bytes().size();
      RecomputeAndPublish("image");
    });
    return audit_sink_ok && telemetry_ok && track_ok && heartbeat_ok && image_ok;
  }

  void OnRunIteration() override {
    if (evaluation_count_ > 0 && evaluation_count_ % 40 == 0) {
      PublishAudit("risk_periodic_report");
    }
  }

  void RecomputeAndPublish(const std::string& trigger) {
    ++evaluation_count_;
    const int congestion = latest_object_count_ * 5 + latest_ego_speed_ / 2;
    const int reliability_gap = std::max(0, 100 - latest_quality_) + std::max(0, 80 - latest_stability_);
    const int image_gap =
        std::max(0, 100 - latest_image_quality_) + std::max(0, 80 - latest_image_stability_);
    const int score = std::clamp(
        congestion + reliability_gap + image_gap / 2 + coordinator_epoch_ % 13,
        0,
        100);
    std::ostringstream risk;
    risk << "seq=" << evaluation_count_ << ";score=" << score << ";congestion=" << congestion
         << ";reliability_gap=" << reliability_gap << ";image_gap=" << image_gap
         << ";trigger=" << trigger << ";";
    Publish("RiskScore", ToPayload(risk.str()));

    if (score >= 75) {
      std::ostringstream alert;
      alert << "seq=" << evaluation_count_ << ";level=" << (score >= 90 ? 3 : 2)
            << ";cause=" << trigger << ";score=" << score << ";";
      Publish("DiagnosisAlert", ToPayload(alert.str()));
    }
    PublishAudit("risk_update");
  }

  void PublishAudit(const std::string& stage) {
    std::ostringstream audit;
    audit << "module=RiskEvalModule;stage=" << stage << ";eval_count=" << evaluation_count_
          << ";quality=" << latest_quality_ << ";objects=" << latest_object_count_
          << ";image_bytes=" << latest_image_size_bytes_ << ";";
    Publish("RiskEvalAuditEvent", ToPayload(audit.str()));
  }

  int latest_quality_ = 90;
  int latest_stability_ = 70;
  int latest_object_count_ = 6;
  int latest_ego_speed_ = 20;
  int coordinator_epoch_ = 0;
  int latest_image_quality_ = 90;
  int latest_image_stability_ = 70;
  std::size_t latest_image_size_bytes_ = 0;
  int evaluation_count_ = 0;
};

REGISTER_MOULD_MODULE_AS("RiskEvalModule", RiskEvalModule)

}  // namespace mould::backend
