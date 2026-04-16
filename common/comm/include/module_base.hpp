#pragma once

#include "callback_queue.hpp"
#include "channel_factory.hpp"
#include "interfaces.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace mould::comm {

struct ModuleRuntimeConfig {
  BusKind bus_kind = BusKind::kSingleNodeShm;
  std::chrono::milliseconds loop_idle_wait{2};
};

struct ModuleRuntimeContext {
  ModuleRuntimeConfig config;
  std::shared_ptr<IPubSubBus> shared_bus;
  std::function<std::unique_ptr<TimerScheduler>()> timer_scheduler_factory;
};

class InlineTimerScheduler final : public TimerScheduler {
 public:
  TimerId RegisterPeriodic(std::chrono::milliseconds interval, Task task) override;
  void Cancel(TimerId timer_id) override;
  void PumpDueTimers() override;

 private:
  struct TimerEntry {
    TimerId id;
    std::chrono::milliseconds interval;
    Clock::time_point next_fire_at;
    Task task;
    bool active = true;
  };

  std::mutex mutex_;
  TimerId next_id_ = 1;
  std::vector<TimerEntry> timers_;
};

class ModuleBase {
 public:
  explicit ModuleBase(std::string module_name, ModuleRuntimeContext runtime_context = {});
  virtual ~ModuleBase() = default;

  bool Run();
  void Stop();

 protected:
  virtual bool Init();
  virtual bool RegisterPublications();
  virtual bool RegisterSubscriptions();
  virtual bool RegisterTimers();
  virtual void OnRunIteration();

  bool AddPublication(const std::string& channel);
  bool AddSubscription(const std::string& channel, IPubSubBus::MessageHandler handler);
  TimerScheduler::TimerId AddPeriodicTimer(
      std::chrono::milliseconds interval,
      std::function<void()> callback);
  bool Publish(const std::string& channel, ByteBuffer payload);

  // Test-only/manual wiring constructor.
  explicit ModuleBase(
      std::string module_name,
      std::shared_ptr<IPubSubBus> bus,
      std::unique_ptr<TimerScheduler> timer_scheduler = std::make_unique<InlineTimerScheduler>());

 private:
  enum class LifecycleStage {
    kCreated,
    kInit,
    kRegisterPublications,
    kRegisterSubscriptions,
    kRegisterTimers,
    kRunning,
    kStopped,
  };

  bool RequireStage(LifecycleStage expected_stage) const;
  void EnsureMainThread() const;

  std::string module_name_;
  ModuleRuntimeConfig runtime_config_;
  std::shared_ptr<IPubSubBus> bus_;
  std::unique_ptr<TimerScheduler> timer_scheduler_;
  CallbackQueue callback_queue_;

  std::atomic<bool> running_{false};
  std::thread::id main_thread_id_;
  LifecycleStage stage_ = LifecycleStage::kCreated;
};

}  // namespace mould::comm
