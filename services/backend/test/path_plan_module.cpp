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

class PathPlanModule final : public ModuleBase {
 public:
  explicit PathPlanModule(const ModuleFactoryConfig& config)
      : ModuleBase(config.module_name, config.runtime_context) {}

 private:
  bool DoInit() override { return true; }

  bool SetupSubscriptions() override {
    const bool command_hint_sink_ok =
        SubscribeOneChannel("CommandHint", [](const MessageEnvelope& /*message*/) {});
    const bool audit_sink_ok =
        SubscribeOneChannel("PathPlanAuditEvent", [](const MessageEnvelope& /*message*/) {});
    const bool track_ok = SubscribeOneChannel("PerceptionTrack", [this](const MessageEnvelope& message) {
      const std::string packet = ToText(message.payload);
      latest_objects_ = ReadFieldInt(packet, "objects", latest_objects_);
      ReplanAndPublish("track");
    });
    const bool risk_ok = SubscribeOneChannel("RiskScore", [this](const MessageEnvelope& message) {
      const std::string packet = ToText(message.payload);
      latest_risk_score_ = ReadFieldInt(packet, "score", latest_risk_score_);
      ReplanAndPublish("risk");
    });
    const bool heartbeat_ok = SubscribeOneChannel("SyncHeartbeat", [this](const MessageEnvelope& message) {
      const std::string packet = ToText(message.payload);
      coordinator_epoch_ = ReadFieldInt(packet, "epoch", coordinator_epoch_);
      ReplanAndPublish("heartbeat");
    });
    const bool image_ok = SubscribeOneChannel("ImageFrame", [](const MessageEnvelope& message) {
      mould::common::image::ImageFrame frame;
      if (!frame.ParseFromArray(message.payload.data(), static_cast<int>(message.payload.size()))) {
        return;
      }
      (void)image_frame_channel::SaveReceivedImageFrameFromMessage(frame, "PathPlanModule");
    });
    return command_hint_sink_ok && audit_sink_ok && track_ok && risk_ok && heartbeat_ok && image_ok;
  }

  void OnRunIteration() override {
    ++on_run_iteration_counter_;
    LOG(INFO) << "[PathPlanModule] on_run_iteration_counter=" << on_run_iteration_counter_;
    if (plan_count_ > 0 && plan_count_ % 30 == 0) {
      PublishAudit("plan_periodic_report");
    }
  }

  void ReplanAndPublish(const std::string& trigger) {
    ++plan_count_;
    const int lane = std::clamp(3 - latest_risk_score_ / 35, 1, 3);
    const int speed_limit = std::clamp(60 - latest_risk_score_ / 2 - latest_objects_, 10, 70);
    const int curvature = std::clamp(8 + latest_objects_ + coordinator_epoch_ % 11, 8, 35);

    std::ostringstream traj;
    traj << "seq=" << plan_count_ << ";lane=" << lane << ";speed_limit=" << speed_limit
         << ";curvature=" << curvature << ";trigger=" << trigger << ";";
    Publish("PlanTrajectory", ToPayload(traj.str()));

    std::ostringstream command_hint;
    command_hint << "seq=" << plan_count_ << ";throttle=" << std::clamp(speed_limit / 4, 5, 20)
                 << ";steer=" << std::clamp(curvature / 2, 4, 18) << ";source=PathPlanModule;";
    Publish("CommandHint", ToPayload(command_hint.str()));
    PublishAudit("plan_update");
  }

  void PublishAudit(const std::string& stage) {
    std::ostringstream audit;
    audit << "module=PathPlanModule;stage=" << stage << ";plan_count=" << plan_count_
          << ";risk=" << latest_risk_score_ << ";objects=" << latest_objects_ << ";";
    Publish("PathPlanAuditEvent", ToPayload(audit.str()));
  }

  int latest_objects_ = 6;
  int latest_risk_score_ = 20;
  int coordinator_epoch_ = 0;
  int plan_count_ = 0;
  std::uint64_t on_run_iteration_counter_ = 0;
};

REGISTER_MOULD_MODULE_AS("PathPlanModule", PathPlanModule)

}  // namespace mould::backend
