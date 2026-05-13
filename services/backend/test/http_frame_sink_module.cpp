#include "gateway_frame_ingress.pb.h"
#include "module_factory_registry.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <glog/logging.h>
#include <string>
#include <thread>

namespace mould::backend {
using mould::comm::ByteBuffer;
using mould::comm::MessageEnvelope;
using mould::comm::ModuleBase;
using mould::comm::ModuleFactoryConfig;

/// 消费 ImageFrameIngress 通道，接收网关发布的帧数据。
///
/// 验证 proto 格式并将图片保存到本地磁盘，用于集成测试中验证
/// HttpGatewayModule → SHM 发布 → 下游消费的端到端链路。
class HttpFrameSinkModule final : public ModuleBase {
 public:
  explicit HttpFrameSinkModule(const ModuleFactoryConfig& config)
      : ModuleBase(config.module_name, config.runtime_context) {
    recv_dir_ = std::filesystem::current_path() / "gateway_integration_recv";
  }

 private:
  bool DoInit() override {
    std::error_code ec;
    std::filesystem::create_directories(recv_dir_, ec);
    if (ec) {
      LOG(ERROR) << "[HttpFrameSinkModule] create recv dir failed: " << ec.message();
      return false;
    }
    LOG(INFO) << "[HttpFrameSinkModule] images will be saved to " << recv_dir_.string();
    return true;
  }

  bool SetupSubscriptions() override {
    return SubscribeOneChannel("ImageFrameIngress",
        [this](const MessageEnvelope& message) {
          OnFrameReceived(message);
        });
  }

  void OnRunIteration() override {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_report_ >= std::chrono::seconds(5)) {
      const auto count = frame_count_.load(std::memory_order_relaxed);
      const auto bytes = total_bytes_.load(std::memory_order_relaxed);
      LOG(INFO) << "[HttpFrameSinkModule] stats: frames=" << count
                << " total_bytes=" << bytes;
      last_report_ = now;
    }
  }

  void OnFrameReceived(const MessageEnvelope& message) {
    ::mould::gateway::ImageFrameIngress frame;
    if (!frame.ParseFromArray(message.payload.data(),
                              static_cast<int>(message.payload.size()))) {
      LOG(WARNING) << "[HttpFrameSinkModule] failed to parse ImageFrameIngress proto"
                   << " payload_size=" << message.payload.size();
      return;
    }

    const auto seq = frame_count_.fetch_add(1, std::memory_order_relaxed);
    total_bytes_.fetch_add(message.payload.size(), std::memory_order_relaxed);

    LOG(INFO) << "[HttpFrameSinkModule] frame #" << seq
              << " request_id=" << frame.request_id()
              << " node_id=" << frame.node_id()
              << " format=" << frame.image_format()
              << " image_bytes=" << frame.image_data().size()
              << " extra_metadata_count=" << frame.extra_metadata_size();

    // 图片数据写入磁盘便于后续人工校验
    const std::string ext = frame.image_format().empty() ? ".bin"
                           : "." + frame.image_format();
    const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto out_path = recv_dir_ / (std::to_string(now_us) + "_" + std::to_string(seq) + ext);
    // std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    // if (out) {
    //   out.write(frame.image_data().data(),
    //             static_cast<std::streamsize>(frame.image_data().size()));
    //   LOG(INFO) << "[HttpFrameSinkModule] saved to " << out_path.string();
    // } else {
    //   LOG(WARNING) << "[HttpFrameSinkModule] failed to write " << out_path.string();
    // }
  }

  std::filesystem::path recv_dir_;
  std::atomic<std::uint64_t> frame_count_{0};
  std::atomic<std::uint64_t> total_bytes_{0};
  std::chrono::steady_clock::time_point last_report_{std::chrono::steady_clock::now()};
};

REGISTER_MOULD_MODULE_AS("HttpFrameSinkModule", HttpFrameSinkModule)

}  // namespace mould::backend
