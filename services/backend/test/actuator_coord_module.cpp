#include "image_frame_channel_helpers.hpp"
#include "module_factory_registry.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <thread>

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

class ActuatorCoordModule final : public ModuleBase {
 public:
  explicit ActuatorCoordModule(const ModuleFactoryConfig& config)
      : ModuleBase(config.module_name, config.runtime_context) {}
  ~ActuatorCoordModule() override {
    running_.store(false, std::memory_order_release);
    if (heartbeat_thread_.joinable()) {
      heartbeat_thread_.join();
    }
  }

 private:
  bool DoInit() override {
    running_.store(true, std::memory_order_release);
    heartbeat_thread_ = std::thread([this]() {
      while (running_.load(std::memory_order_acquire)) {
        ++epoch_;
        std::ostringstream heartbeat;
        heartbeat << "epoch=" << epoch_ << ";state=" << (fault_level_ > 1 ? "degraded" : "normal")
                  << ";last_cmd=" << command_count_ << ";";
        Publish("SyncHeartbeat", ToPayload(heartbeat.str()));
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
      }
    });
    return true;
  }

  bool SetupSubscriptions() override {
    const bool audit_sink_ok =
        SubscribeOneChannel("ActuatorCoordAuditEvent", [](const MessageEnvelope& /*message*/) {});
    const bool plan_ok = SubscribeOneChannel("PlanTrajectory", [this](const MessageEnvelope& message) {
      const std::string packet = ToText(message.payload);
      planned_speed_limit_ = ReadFieldInt(packet, "speed_limit", planned_speed_limit_);
      planned_curvature_ = ReadFieldInt(packet, "curvature", planned_curvature_);
      EmitCommand("trajectory");
    });
    const bool risk_ok = SubscribeOneChannel("RiskScore", [this](const MessageEnvelope& message) {
      const std::string packet = ToText(message.payload);
      latest_risk_score_ = ReadFieldInt(packet, "score", latest_risk_score_);
      EmitCommand("risk");
    });
    const bool alert_ok = SubscribeOneChannel("DiagnosisAlert", [this](const MessageEnvelope& message) {
      const std::string packet = ToText(message.payload);
      fault_level_ = ReadFieldInt(packet, "level", fault_level_);
      EmitCommand("alert");
    });
    const bool image_ok = SubscribeOneChannel("ImageFrame", [](const MessageEnvelope& message) {
      mould::common::image::ImageFrame frame;
      if (!frame.ParseFromArray(message.payload.data(), static_cast<int>(message.payload.size()))) {
        return;
      }
      (void)image_frame_channel::SaveReceivedImageFrameFromMessage(frame, "ActuatorCoordModule");
    });
    return audit_sink_ok && plan_ok && risk_ok && alert_ok && image_ok;
  }

  void EmitCommand(const std::string& trigger) {
    ++command_count_;
    const int capped_speed = std::max(5, planned_speed_limit_ - latest_risk_score_ / 4 - fault_level_ * 8);
    const int throttle = std::clamp(capped_speed / 3, 3, 25);
    const int brake = std::clamp((latest_risk_score_ + fault_level_ * 15) / 6, 0, 18);
    const int steer = std::clamp(planned_curvature_ / 2 + fault_level_ * 2, 4, 20);

    std::ostringstream command;
    command << "seq=" << command_count_ << ";throttle=" << throttle << ";brake=" << brake
            << ";steer=" << steer << ";trigger=" << trigger << ";";
    Publish("ActuatorCommand", ToPayload(command.str()));
    PublishAudit(trigger, throttle, brake, steer);
  }

  void PublishAudit(const std::string& trigger, int throttle, int brake, int steer) {
    std::ostringstream audit;
    audit << "module=ActuatorCoordModule;trigger=" << trigger << ";cmd_seq=" << command_count_
          << ";throttle=" << throttle << ";brake=" << brake << ";steer=" << steer << ";";
    Publish("ActuatorCoordAuditEvent", ToPayload(audit.str()));
  }

  std::atomic<bool> running_{false};
  std::thread heartbeat_thread_;
  int epoch_ = 0;
  int fault_level_ = 0;
  int latest_risk_score_ = 20;
  int planned_speed_limit_ = 50;
  int planned_curvature_ = 12;
  int command_count_ = 0;
};

REGISTER_MOULD_MODULE_AS("ActuatorCoordModule", ActuatorCoordModule)

}  // namespace mould::backend
