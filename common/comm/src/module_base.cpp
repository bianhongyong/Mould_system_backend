#include "module_base.hpp"

#include "shm_bus_runtime.hpp"
#include "shm_bus_runtime_prefork.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace mould::comm {

TimerScheduler::TimerId InlineTimerScheduler::RegisterPeriodic(
    std::chrono::milliseconds interval,
    Task task) {
  if (interval.count() <= 0 || !task) {
    return 0;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const TimerId timer_id = next_id_++;
  timers_.push_back(TimerEntry{
      .id = timer_id,
      .interval = interval,
      .next_fire_at = Clock::now() + interval,
      .task = std::move(task),
      .active = true,
  });
  return timer_id;
}

void InlineTimerScheduler::Cancel(TimerId timer_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (TimerEntry& timer : timers_) {
    if (timer.id == timer_id) {
      timer.active = false;
      break;
    }
  }
}

void InlineTimerScheduler::PumpDueTimers() {
  std::vector<Task> tasks_to_run;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const Clock::time_point now = Clock::now();
    for (TimerEntry& timer : timers_) {
      if (!timer.active || timer.next_fire_at > now) {
        continue;
      }
      timer.next_fire_at = now + timer.interval;
      tasks_to_run.push_back(timer.task);
    }
    timers_.erase(
        std::remove_if(
            timers_.begin(),
            timers_.end(),
            [](const TimerEntry& timer) { return !timer.active; }),
        timers_.end());
  }

  for (Task& task : tasks_to_run) {
    if (task) {
      task();
    }
  }
}

ModuleBase::ModuleBase(
    std::string module_name,
    ModuleRuntimeContext runtime_context)
    : ModuleBase(
          std::move(module_name),
          runtime_context.shared_bus ? runtime_context.shared_bus
                                     : ChannelFactory::CreateShared(runtime_context.config.bus_kind),
          runtime_context.timer_scheduler_factory
              ? runtime_context.timer_scheduler_factory()
              : std::make_unique<InlineTimerScheduler>()) {
  runtime_config_ = runtime_context.config;
}

ModuleBase::ModuleBase(
    std::string module_name,
    std::shared_ptr<IPubSubBus> bus,
    std::unique_ptr<TimerScheduler> timer_scheduler)
    : module_name_(std::move(module_name)),
      runtime_config_(ModuleRuntimeConfig{}),
      bus_(std::move(bus)),
      timer_scheduler_(std::move(timer_scheduler)) {}

bool ModuleBase::Run() {
  if (!bus_ || !timer_scheduler_ || running_.exchange(true)) {
    return false;
  }

  if (runtime_config_.bus_kind == BusKind::kSingleNodeShm) {
    ShmBusRuntimeAssertForkOnlyModelOrDie();
  }

  main_thread_id_ = std::this_thread::get_id();

  stage_ = LifecycleStage::kInit;
  if (!Init()) {
    stage_ = LifecycleStage::kStopped;
    running_.store(false);
    return false;
  }

  stage_ = LifecycleStage::kRegisterPublications;
  if (!RegisterPublications()) {
    stage_ = LifecycleStage::kStopped;
    running_.store(false);
    return false;
  }

  stage_ = LifecycleStage::kRegisterSubscriptions;
  if (!RegisterSubscriptions()) {
    stage_ = LifecycleStage::kStopped;
    running_.store(false);
    return false;
  }

  if (runtime_config_.bus_kind == BusKind::kSingleNodeShm) {
    const auto shm_bus = std::dynamic_pointer_cast<ShmBusRuntime>(bus_);
    if (shm_bus && !shm_bus->StartUnifiedSubscriberPump()) {
      stage_ = LifecycleStage::kStopped;
      running_.store(false);
      return false;
    }
  }

  stage_ = LifecycleStage::kRegisterTimers;
  if (!RegisterTimers()) {
    stage_ = LifecycleStage::kStopped;
    running_.store(false);
    return false;
  }

  stage_ = LifecycleStage::kRunning;
  while (running_.load()) {
    timer_scheduler_->PumpDueTimers();
    callback_queue_.WaitAndDrain(runtime_config_.loop_idle_wait);
    OnRunIteration();
  }

  callback_queue_.DrainAll();
  stage_ = LifecycleStage::kStopped;
  return true;
}

void ModuleBase::Stop() {
  running_.store(false);
}

bool ModuleBase::Init() {
  return true;
}

bool ModuleBase::RegisterPublications() {
  return true;
}

bool ModuleBase::RegisterSubscriptions() {
  return true;
}

bool ModuleBase::RegisterTimers() {
  return true;
}

void ModuleBase::OnRunIteration() {}

bool ModuleBase::AddPublication(const std::string& channel) {
  if (!RequireStage(LifecycleStage::kRegisterPublications)) {
    return false;
  }
  return bus_->RegisterPublisher(module_name_, channel);
}

bool ModuleBase::AddSubscription(const std::string& channel, IPubSubBus::MessageHandler handler) {
  if (!RequireStage(LifecycleStage::kRegisterSubscriptions) || !handler) {
    return false;
  }

  return bus_->Subscribe(
      module_name_,
      channel,
      [this, handler = std::move(handler)](const MessageEnvelope& message) {
        callback_queue_.Push([this, handler, message]() {
          EnsureMainThread();
          handler(message);
        });
      });
}

TimerScheduler::TimerId ModuleBase::AddPeriodicTimer(
    std::chrono::milliseconds interval,
    std::function<void()> callback) {
  if (!RequireStage(LifecycleStage::kRegisterTimers) || !callback) {
    return 0;
  }

  return timer_scheduler_->RegisterPeriodic(
      interval,
      [this, callback = std::move(callback)]() {
        callback_queue_.Push([this, callback]() {
          EnsureMainThread();
          callback();
        });
      });
}

bool ModuleBase::Publish(const std::string& channel, ByteBuffer payload) {
  if (!RequireStage(LifecycleStage::kRunning)) {
    return false;
  }
  return bus_->Publish(module_name_, channel, std::move(payload));
}

bool ModuleBase::RequireStage(LifecycleStage expected_stage) const {
  return stage_ == expected_stage;
}

void ModuleBase::EnsureMainThread() const {
  if (std::this_thread::get_id() != main_thread_id_) {
    throw std::runtime_error("Callback executed outside module main thread");
  }
}

}  // namespace mould::comm
