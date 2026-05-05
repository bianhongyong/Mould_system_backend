#include "module_base.hpp"

#include "shm_bus_runtime.hpp"
#include "shm_bus_runtime_prefork.hpp"
#include "channel_topology_config.hpp"

#include <algorithm>
#include <glog/logging.h>
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
    LOG(ERROR) << "ModuleBase::Run precheck failed, module=" << module_name_ << " bus=" << (bus_ != nullptr)
               << " timer_scheduler=" << (timer_scheduler_ != nullptr)
               << " already_running=" << running_.load();
    return false;
  }

  if (runtime_config_.bus_kind == BusKind::kSingleNodeShm) {
    ShmBusRuntimeAssertForkOnlyModelOrDie();
  }

  main_thread_id_ = std::this_thread::get_id();

  stage_ = LifecycleStage::kInit;
  if (!Init()) {
    LOG(ERROR) << "ModuleBase::Init failed, module=" << module_name_;
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
  declared_subscription_handlers_.clear();
  stage_ = LifecycleStage::kSetupSubscriptions;
  if (!SetupSubscriptions()) {
    LOG(ERROR) << "ModuleBase::SetupSubscriptions failed, module=" << module_name_;
    return false;
  }
  const auto null_handler = [](const MessageEnvelope&) {};

  if (runtime_config_.bus_kind == BusKind::kSingleNodeShm) {
    auto shm_bus = std::dynamic_pointer_cast<ShmBusRuntime>(bus_);
    if (!shm_bus) {
      shm_bus = std::make_shared<ShmBusRuntime>();
      bus_ = shm_bus;
    }

    if (!runtime_config_.module_config_files.empty()) {
      std::string topology_error;
      if (!shm_bus->SetChannelTopologyFromModuleConfigs(
              runtime_config_.module_config_files,
              &topology_error)) {
        LOG(ERROR) << "SetChannelTopologyFromModuleConfigs failed, module=" << module_name_
                   << " error=" << topology_error;
        return false;
      }
    }
  }

  config::ModuleChannelConfig module_channel_config;
  std::string parse_error;
  if (!runtime_config_.module_channel_config_path.empty()) {
    if (!config::ParseModuleChannelConfigFile(
            module_name_,
            runtime_config_.module_channel_config_path,
            &module_channel_config,
            &parse_error)) {
      LOG(ERROR) << "ParseModuleChannelConfigFile failed, module=" << module_name_
                 << " path=" << runtime_config_.module_channel_config_path
                 << " error=" << parse_error;
      return false;
    }
  } else {
    module_channel_config.module_name = module_name_;
  }

  for (const auto& endpoint : module_channel_config.input_channels) {
    auto it = declared_subscription_handlers_.find(endpoint.channel);
    const auto user_handler =
        (it != declared_subscription_handlers_.end() && it->second) ? it->second : null_handler;
    if (!bus_->Subscribe(
            module_name_,
            endpoint.channel,
            [this, user_handler](const MessageEnvelope& message) {
              callback_queue_.Push([this, user_handler, message]() {
                EnsureMainThread();
                user_handler(message);
              });
            })) {
      LOG(ERROR) << "Subscribe failed, module=" << module_name_ << " channel=" << endpoint.channel;
      return false;
    }
  }

  if (runtime_config_.bus_kind == BusKind::kSingleNodeShm) {
    const auto shm_bus = std::dynamic_pointer_cast<ShmBusRuntime>(bus_);
    if (shm_bus && !shm_bus->StartUnifiedSubscriberPump()) {
      LOG(ERROR) << "StartUnifiedSubscriberPump failed, module=" << module_name_;
      return false;
    }
  }

  if (!DoInit()) {
    LOG(ERROR) << "ModuleBase::DoInit failed, module=" << module_name_;
    return false;
  }

  return true;
}

void ModuleBase::OnRunIteration() {}

bool ModuleBase::SubscribeOneChannel(
    const std::string& channel,
    IPubSubBus::MessageHandler handler) {
  if (!RequireStage(LifecycleStage::kSetupSubscriptions) || channel.empty() || !handler) {
    return false;
  }
  declared_subscription_handlers_[channel] = std::move(handler);
  return true;
}

bool ModuleBase::Publish(const std::string& channel, ByteBuffer payload) {
  auto publish_status = PublishWithStatus(channel, std::move(payload));
  if (!publish_status.ok()) {
    LOG(WARNING) << "ModuleBase::Publish failed, module=" << module_name_
                 << " channel=" << channel
                 << " reason=" << publish_status.status();
    return false;
  }
  return true;
}

absl::StatusOr<std::uint64_t> ModuleBase::PublishWithStatus(const std::string& channel, ByteBuffer payload) {
  if (!RequireStage(LifecycleStage::kRunning)) {
    return absl::FailedPreconditionError("module is not in running stage");
  }
  if (!bus_) {
    return absl::FailedPreconditionError("bus is null");
  }
  auto publish_result = bus_->PublishWithStatus(module_name_, channel, std::move(payload));
  if (!publish_result.ok()) {
    LOG(WARNING) << "ModuleBase::PublishWithStatus failed, module=" << module_name_
                 << " channel=" << channel
                 << " status=" << publish_result.status();
    return publish_result.status();
  }
  return publish_result;
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
