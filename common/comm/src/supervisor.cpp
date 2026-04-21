#include "supervisor.hpp"
#include "ready_pipe_protocol.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
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

}  // namespace mould::comm
