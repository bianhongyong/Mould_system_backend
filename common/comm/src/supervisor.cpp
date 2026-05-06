#include "supervisor.hpp"
#include "launch_plan_config.hpp"
#include "ready_pipe_protocol.hpp"

#include <glog/logging.h>

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cerrno>
#include <thread>
#include <unordered_set>
#include <utility>

namespace mould::comm {

Supervisor::Supervisor(std::uint32_t random_seed) : rng_(random_seed) {}

bool Supervisor::ValidateSingleModulePerProcessInvariant(
    const std::vector<SupervisorModuleSpec>& modules,
    std::string* out_error) const {
  std::unordered_set<std::string> seen_modules;
  for (const auto& spec : modules) {
    if (spec.module_name.empty()) {
      if (out_error != nullptr) {
        *out_error = "supervisor module entry has empty module_name";
      }
      return false;
    }
    if (!seen_modules.insert(spec.module_name).second) {
      if (out_error != nullptr) {
        *out_error = "duplicate module in launch plan: " + spec.module_name;
      }
      return false;
    }
  }
  return true;
}

std::vector<std::vector<SupervisorModuleSpec>> Supervisor::BuildInitialStartupBatches(
    const std::vector<SupervisorModuleSpec>& modules) const {
  std::unordered_map<std::int64_t, std::vector<SupervisorModuleSpec>> grouped;
  grouped.reserve(modules.size());
  for (const auto& spec : modules) {
    grouped[spec.startup_priority].push_back(spec);
  }

  std::vector<std::int64_t> priorities;
  priorities.reserve(grouped.size());
  for (const auto& [priority, specs] : grouped) {
    (void)specs;
    priorities.push_back(priority);
  }
  std::sort(priorities.begin(), priorities.end());

  std::vector<std::vector<SupervisorModuleSpec>> batches;
  batches.reserve(priorities.size());
  for (const std::int64_t priority : priorities) {
    auto batch = grouped.at(priority);
    std::shuffle(batch.begin(), batch.end(), rng_);
    for (const auto& module : batch) {
      observability_.startup_order_trace.push_back(module.module_name);
    }
    batches.push_back(std::move(batch));
  }
  return batches;
}

std::vector<SupervisorModuleSpec> Supervisor::OrderSamePriorityRestartBatch(
    const std::vector<SupervisorModuleSpec>& pending_restart) const {
  std::vector<SupervisorModuleSpec> ordered = pending_restart;
  if (ordered.size() > 1U) {
    std::shuffle(ordered.begin(), ordered.end(), rng_);
  }
  for (const auto& module : ordered) {
    observability_.restart_order_trace.push_back(module.module_name);
  }
  return ordered;
}

bool Supervisor::ForkModuleProcess(
    const std::string& module_name,
    const ChildEntrypoint& child_entrypoint,
    std::string* out_error) {
  if (module_name.empty()) {
    if (out_error != nullptr) {
      *out_error = "cannot fork module with empty module_name";
    }
    return false;
  }
  if (!child_entrypoint) {
    if (out_error != nullptr) {
      *out_error = "cannot fork module without child entrypoint";
    }
    return false;
  }
  if (module_to_pid_.find(module_name) != module_to_pid_.end()) {
    if (out_error != nullptr) {
      *out_error = "module already has an active child process: " + module_name;
    }
    return false;
  }

  const pid_t child = fork();
  if (child < 0) {
    if (out_error != nullptr) {
      *out_error = "fork failed for module: " + module_name;
    }
    return false;
  }
  if (child == 0) {
    const int entry_code = child_entrypoint();
    _exit(entry_code == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
  }

  module_to_pid_[module_name] = child;
  pid_to_module_[child] = module_name;
  TransitionToForked(module_name);
  return true;
}

bool Supervisor::HasChildProcess(const std::string& module_name) const {
  return module_to_pid_.find(module_name) != module_to_pid_.end();
}

std::optional<pid_t> Supervisor::ChildPidOf(const std::string& module_name) const {
  const auto it = module_to_pid_.find(module_name);
  if (it == module_to_pid_.end()) {
    return std::nullopt;
  }
  return it->second;
}

bool Supervisor::ReapChildProcess(pid_t pid) {
  auto pid_it = pid_to_module_.find(pid);
  if (pid_it == pid_to_module_.end()) {
    return false;
  }
  const auto module_it = module_to_pid_.find(pid_it->second);
  if (module_it != module_to_pid_.end() && module_it->second == pid) {
    module_to_pid_.erase(module_it);
  }
  pid_to_module_.erase(pid_it);
  return true;
}

void Supervisor::TransitionToForked(const std::string& module_name) {
  module_lifecycle_[module_name] = ModuleLifecycleRecord{
      .state = ModuleLifecycleState::kForked,
      .failure_reason = "",
  };
}

void Supervisor::TransitionToIniting(const std::string& module_name) {
  module_lifecycle_[module_name] = ModuleLifecycleRecord{
      .state = ModuleLifecycleState::kIniting,
      .failure_reason = "",
  };
}

void Supervisor::TransitionToReady(const std::string& module_name) {
  module_lifecycle_[module_name] = ModuleLifecycleRecord{
      .state = ModuleLifecycleState::kReady,
      .failure_reason = "",
  };
  observability_.ready_transitions[module_name] += 1;
}

void Supervisor::TransitionToRunning(const std::string& module_name) {
  module_lifecycle_[module_name] = ModuleLifecycleRecord{
      .state = ModuleLifecycleState::kRunning,
      .failure_reason = "",
  };
}

void Supervisor::TransitionToFailed(const std::string& module_name, std::string failure_reason) {
  module_lifecycle_[module_name] = ModuleLifecycleRecord{
      .state = ModuleLifecycleState::kFailed,
      .failure_reason = std::move(failure_reason),
  };
}

std::optional<ModuleLifecycleRecord> Supervisor::LifecycleOf(const std::string& module_name) const {
  const auto it = module_lifecycle_.find(module_name);
  if (it == module_lifecycle_.end()) {
    return std::nullopt;
  }
  return it->second;
}

bool Supervisor::WaitForReadyOrTransitionFailed(
    const std::string& module_name,
    int parent_ready_pipe_fd,
    std::int64_t ready_timeout_ms) {
  TransitionToIniting(module_name);
  std::string error;
  if (ReadyPipeProtocol::WaitReady(parent_ready_pipe_fd, ready_timeout_ms, &error)) {
    TransitionToReady(module_name);
    return true;
  }
  TransitionToFailed(module_name, error);
  return false;
}

bool Supervisor::CanReleaseNextPriorityBatch(const std::vector<std::string>& current_batch_modules) const {
  for (const auto& module_name : current_batch_modules) {
    const auto state = LifecycleOf(module_name);
    if (!state.has_value()) {
      return false;
    }
    if (state->state != ModuleLifecycleState::kReady && state->state != ModuleLifecycleState::kFailed) {
      return false;
    }
  }
  return true;
}

RestartDecision Supervisor::HandleAbnormalChildExit(
    const std::string& module_name,
    const RestartPolicyConfig& policy_config,
    std::int64_t now_ms) {
  TransitionToFailed(module_name, "abnormal child exit");
  observability_.restart_evaluations[module_name] += 1;
  const RestartDecision decision = restart_policy_.EvaluateAbnormalExit(module_name, policy_config, now_ms);
  if (decision.fuse_open) {
    observability_.fuse_open_events[module_name] += 1;
  }
  // Non-fast-fail contract: even when fuse is open, supervisor keeps master process alive.
  master_alive_ = true;
  return decision;
}

bool Supervisor::IsMasterAlive() const {
  return master_alive_;
}

SupervisorObservabilitySnapshot Supervisor::ObservabilitySnapshot() const {
  return observability_;
}

void Supervisor::RunMonitorLoop(MonitorDeps& deps) {
  while (!deps.shutdown_requested->load()) {
    int status = 0;
    const pid_t pid = waitpid(-1, &status, WNOHANG);
    if (pid == 0) {
      std::this_thread::sleep_for(deps.idle_sleep_duration);
      continue;
    }
    if (pid < 0) {
      if (errno == EINTR) {
        continue;
      }
      LOG(WARNING) << "[backend_main] waitpid returned error errno=" << errno
                   << ", leaving monitor loop";
      break;
    }

    // ---- Log module exit: full pipe topology rebuild ----
    if (deps.has_log_module && deps.mutable_log_module_pid != nullptr &&
        pid == *deps.mutable_log_module_pid) {
      LOG(ERROR) << "[backend_main] log module exited, rebuilding pipe topology";
      ReapChildProcess(pid);

      if (!deps.shutdown_requested->load() && deps.restart_log_module) {
        std::string error;
        if (!deps.restart_log_module(&error)) {
          LOG(FATAL) << "[backend_main] log module restart failed: " << error;
        }
      }
      continue;
    }

    // ---- Business module exit ----
    auto pid_it = pid_to_module_.find(pid);
    if (pid_it == pid_to_module_.end()) {
      LOG(WARNING) << "[backend_main] received child exit for unmanaged pid=" << pid;
      continue;
    }
    const std::string module_name = pid_it->second;

    // SHM: take all consumers of this PID offline before reaping.
    if (deps.take_consumers_offline) {
      const std::uint32_t shm_offline_count =
          deps.take_consumers_offline(static_cast<std::uint32_t>(pid));
      LOG(INFO) << "[backend_main] shm consumers taken offline for exited pid=" << pid
                << " module=" << module_name << " count=" << shm_offline_count;
    }

    ReapChildProcess(pid);
    LOG(ERROR) << "[backend_main] child exited module=" << module_name
               << " pid=" << pid << " raw_status=" << status;

    // Clean exit → no restart.
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
      LOG(INFO) << "[backend_main] module exited cleanly, no restart module=" << module_name;
      continue;
    }

    // Look up module entry for restart policy.
    if (deps.module_entries == nullptr) {
      continue;
    }
    const auto entry_it = deps.module_entries->find(module_name);
    if (entry_it == deps.module_entries->end()) {
      continue;
    }
    const auto& schema = entry_it->second.resource_schema;
    const RestartPolicyConfig policy{
        .restart_backoff_ms = schema.restart_backoff_ms,
        .restart_max_retries = schema.restart_max_retries,
        .restart_window_ms = schema.restart_window_ms,
        .restart_fuse_ms = schema.restart_fuse_ms,
    };
    const std::int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const RestartDecision decision = HandleAbnormalChildExit(module_name, policy, now_ms);
    LOG(INFO) << "[backend_main] restart decision module=" << module_name
              << " should_restart=" << decision.should_restart
              << " fuse_open=" << decision.fuse_open
              << " delay_ms=" << decision.restart_delay_ms;

    if (decision.should_restart) {
      if (decision.restart_delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(decision.restart_delay_ms));
      }
      if (deps.restart_business_module) {
        std::string restart_error;
        if (!deps.restart_business_module(module_name, &restart_error)) {
          LOG(ERROR) << "[backend_main] restart launch failed module=" << module_name
                     << " error=" << restart_error;
        } else {
          LOG(INFO) << "[backend_main] restart launch success module=" << module_name;
        }
      }
    } else if (decision.fuse_open) {
      LOG(INFO) << "[backend_main] fuse open for module=" << module_name
                << ", closing log pipe write_fd";
      if (deps.close_log_pipe) {
        deps.close_log_pipe(module_name);
      }
    }
  }
}

}  // namespace mould::comm
