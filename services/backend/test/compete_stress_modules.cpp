#include "module_factory_registry.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <glog/logging.h>
#include <mutex>
#include <string>
#include <thread>

namespace mould::backend {
using mould::comm::ByteBuffer;
using mould::comm::MessageEnvelope;
using mould::comm::ModuleBase;
using mould::comm::ModuleFactoryConfig;

namespace {

constexpr std::uint32_t kPayloadMagic = 0xC0FFEE01U;
/// 单次压测目标时长（与 `kPublishIntervalMicros` 共同决定总条数）。
constexpr int kStressRunDurationSeconds = 60;
/// 发布间隔：改小可提高环上竞争（更易触发背压）；为 0 时不 sleep，极限抢跑（CPU 占用高）。
/// 若频繁 `Publish failed` 多为槽位/消费跟不上，可增大 `launch_plan_compete_stress.json` 里 `shm_slot_count`
/// 或略增大间隔以区分「背压」与 compete 语义问题。
constexpr int kPublishIntervalMicros = 15;
/// 总发布条数 ≈ `kStressRunDurationSeconds * 1e6 / interval_us`（例如 60s、15us → 4,000,000）。
constexpr std::uint64_t kPublishCount =
    (kPublishIntervalMicros > 0)
        ? (static_cast<std::uint64_t>(kStressRunDurationSeconds) * 1'000'000ULL /
           static_cast<std::uint64_t>(kPublishIntervalMicros))
        : 4'000'000ULL;
constexpr const char kRelOutDir[] = "compete_stress_out";

std::filesystem::path OutDir() {
  return std::filesystem::current_path() / kRelOutDir;
}

std::uint32_t Crc32(std::uint64_t seq) {
  return static_cast<std::uint32_t>(seq ^ (seq >> 32) ^ 0xA5A5A5A5ULL ^ static_cast<std::uint64_t>(kPayloadMagic));
}

#pragma pack(push, 1)
struct WirePayload {
  std::uint32_t magic = 0;
  std::uint64_t seq = 0;
  std::uint32_t crc32 = 0;
};
#pragma pack(pop)

static_assert(sizeof(WirePayload) == 16, "WirePayload size");

ByteBuffer SerializeSeq(std::uint64_t seq) {
  WirePayload w{};
  w.magic = kPayloadMagic;
  w.seq = seq;
  w.crc32 = Crc32(seq);
  ByteBuffer out(sizeof(WirePayload));
  std::memcpy(out.data(), &w, sizeof(WirePayload));
  return out;
}

bool ParseAndVerify(const ByteBuffer& payload, std::uint64_t* out_seq) {
  if (payload.size() < sizeof(WirePayload) || out_seq == nullptr) {
    return false;
  }
  WirePayload w{};
  std::memcpy(&w, payload.data(), sizeof(WirePayload));
  if (w.magic != kPayloadMagic || w.crc32 != Crc32(w.seq)) {
    return false;
  }
  *out_seq = w.seq;
  return true;
}

}  // namespace

/// 单生产者：`CompeteStress` 通道，`delivery_mode=compete`，按 `kPublishIntervalMicros` 发布单调序号。
class CompeteStressProducer final : public ModuleBase {
 public:
  explicit CompeteStressProducer(const ModuleFactoryConfig& config)
      : ModuleBase(config.module_name, config.runtime_context) {}

  ~CompeteStressProducer() override {
    stop_.store(true, std::memory_order_release);
    if (producer_.joinable()) {
      producer_.join();
    }
  }

 private:
  bool DoInit() override {
    std::error_code ec;
    std::filesystem::create_directories(OutDir(), ec);
    if (ec) {
      LOG(ERROR) << "[CompeteStressProducer] create_directories failed: " << ec.message();
      return false;
    }
    stop_.store(false, std::memory_order_release);
    producer_ = std::thread([this]() {
      // `Publish` 仅在 `kRunning` 阶段合法；等待主线程进入 Run 循环。
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      for (std::uint64_t s = 1; s <= kPublishCount; ++s) {
        if (stop_.load(std::memory_order_acquire)) {
          break;
        }
        ByteBuffer wire = SerializeSeq(s);
        if (!Publish("CompeteStress", std::move(wire))) {
          LOG(ERROR) << "[CompeteStressProducer] Publish failed seq=" << s;
          break;
        }
        if ((s % 500'000U) == 0U) {
          LOG(INFO) << "[CompeteStressProducer] published through seq=" << s;
        }
        if (kPublishIntervalMicros > 0) {
          std::this_thread::sleep_for(std::chrono::microseconds(kPublishIntervalMicros));
        } else {
          std::this_thread::yield();
        }
      }
      {
        std::ofstream meta(OutDir() / "meta.txt", std::ios::out | std::ios::trunc);
        if (meta) {
          meta << "publish_count=" << kPublishCount << "\n";
          meta << "interval_us=" << kPublishIntervalMicros << "\n";
          meta << "target_duration_s=" << kStressRunDurationSeconds << "\n";
          meta << "payload_bytes=" << sizeof(WirePayload) << "\n";
        } else {
          LOG(ERROR) << "[CompeteStressProducer] failed to write meta.txt";
        }
      }
      LOG(INFO) << "[CompeteStressProducer] finished publish_count=" << kPublishCount;
    });
    return true;
  }

  bool SetupSubscriptions() override { return true; }

  std::thread producer_;
  std::atomic<bool> stop_{false};
};

class CompeteStressConsumerA final : public ModuleBase {
 public:
  explicit CompeteStressConsumerA(const ModuleFactoryConfig& config)
      : ModuleBase(config.module_name, config.runtime_context) {}

  ~CompeteStressConsumerA() override = default;

 private:
  bool DoInit() override {
    std::error_code ec;
    std::filesystem::create_directories(OutDir(), ec);
    recv_path_ = OutDir() / "recv_A.bin";
    recv_file_.open(recv_path_, std::ios::binary | std::ios::trunc);
    if (!recv_file_) {
      LOG(ERROR) << "[CompeteStressConsumerA] open recv file failed path=" << recv_path_;
      return false;
    }
    return true;
  }

  bool SetupSubscriptions() override {
    return SubscribeOneChannel("CompeteStress", [this](const MessageEnvelope& message) {
      std::uint64_t seq = 0;
      if (!ParseAndVerify(message.payload, &seq)) {
        LOG(ERROR) << "[CompeteStressConsumerA] bad payload size=" << message.payload.size();
        return;
      }
      std::lock_guard<std::mutex> lock(mu_);
      recv_file_.write(reinterpret_cast<const char*>(&seq), sizeof(seq));
      if (!recv_file_.flush()) {
        LOG(WARNING) << "[CompeteStressConsumerA] flush failed seq=" << seq;
      }
    });
  }

  std::mutex mu_;
  std::ofstream recv_file_;
  std::filesystem::path recv_path_;
};

class CompeteStressConsumerB final : public ModuleBase {
 public:
  explicit CompeteStressConsumerB(const ModuleFactoryConfig& config)
      : ModuleBase(config.module_name, config.runtime_context) {}

 private:
  bool DoInit() override {
    std::error_code ec;
    std::filesystem::create_directories(OutDir(), ec);
    recv_path_ = OutDir() / "recv_B.bin";
    recv_file_.open(recv_path_, std::ios::binary | std::ios::trunc);
    if (!recv_file_) {
      LOG(ERROR) << "[CompeteStressConsumerB] open recv file failed path=" << recv_path_;
      return false;
    }
    return true;
  }

  bool SetupSubscriptions() override {
    return SubscribeOneChannel("CompeteStress", [this](const MessageEnvelope& message) {
      std::uint64_t seq = 0;
      if (!ParseAndVerify(message.payload, &seq)) {
        LOG(ERROR) << "[CompeteStressConsumerB] bad payload size=" << message.payload.size();
        return;
      }
      std::lock_guard<std::mutex> lock(mu_);
      recv_file_.write(reinterpret_cast<const char*>(&seq), sizeof(seq));
      (void)recv_file_.flush();
    });
  }

  std::mutex mu_;
  std::ofstream recv_file_;
  std::filesystem::path recv_path_;
};

class CompeteStressConsumerC final : public ModuleBase {
 public:
  explicit CompeteStressConsumerC(const ModuleFactoryConfig& config)
      : ModuleBase(config.module_name, config.runtime_context) {}

 private:
  bool DoInit() override {
    std::error_code ec;
    std::filesystem::create_directories(OutDir(), ec);
    recv_path_ = OutDir() / "recv_C.bin";
    recv_file_.open(recv_path_, std::ios::binary | std::ios::trunc);
    if (!recv_file_) {
      LOG(ERROR) << "[CompeteStressConsumerC] open recv file failed path=" << recv_path_;
      return false;
    }
    return true;
  }

  bool SetupSubscriptions() override {
    return SubscribeOneChannel("CompeteStress", [this](const MessageEnvelope& message) {
      std::uint64_t seq = 0;
      if (!ParseAndVerify(message.payload, &seq)) {
        LOG(ERROR) << "[CompeteStressConsumerC] bad payload size=" << message.payload.size();
        return;
      }
      std::lock_guard<std::mutex> lock(mu_);
      recv_file_.write(reinterpret_cast<const char*>(&seq), sizeof(seq));
      (void)recv_file_.flush();
    });
  }

  std::mutex mu_;
  std::ofstream recv_file_;
  std::filesystem::path recv_path_;
};

REGISTER_MOULD_MODULE_AS("CompeteStressProducer", CompeteStressProducer)
REGISTER_MOULD_MODULE_AS("CompeteStressConsumerA", CompeteStressConsumerA)
REGISTER_MOULD_MODULE_AS("CompeteStressConsumerB", CompeteStressConsumerB)
REGISTER_MOULD_MODULE_AS("CompeteStressConsumerC", CompeteStressConsumerC)

}  // namespace mould::backend
