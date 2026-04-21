#pragma once

#include "restart_policy.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace mould::comm {

struct SupervisorModuleSpec {
  std::string module_name;
  std::int64_t startup_priority = 0;
};

enum class ModuleLifecycleState {
  kForked,
  kIniting,
  kReady,
  kRunning,
  kFailed,
};

struct ModuleLifecycleRecord {
  ModuleLifecycleState state = ModuleLifecycleState::kForked;
  std::string failure_reason;
};

struct SupervisorObservabilitySnapshot {
  std::unordered_map<std::string, std::uint64_t> ready_transitions;
  std::unordered_map<std::string, std::uint64_t> restart_evaluations;
  std::unordered_map<std::string, std::uint64_t> fuse_open_events;
  std::vector<std::string> startup_order_trace;
  std::vector<std::string> restart_order_trace;
};

class Supervisor {
 public:
  using ChildEntrypoint = std::function<int()>;

  explicit Supervisor(std::uint32_t random_seed = std::random_device{}());

  bool ValidateSingleModulePerProcessInvariant(
      const std::vector<SupervisorModuleSpec>& modules,
      std::string* out_error) const;

  std::vector<std::vector<SupervisorModuleSpec>> BuildInitialStartupBatches(
      const std::vector<SupervisorModuleSpec>& modules) const;

  std::vector<SupervisorModuleSpec> OrderSamePriorityRestartBatch(
      const std::vector<SupervisorModuleSpec>& pending_restart) const;

  bool ForkModuleProcess(
      const std::string& module_name,
      const ChildEntrypoint& child_entrypoint,
      std::string* out_error);

  bool HasChildProcess(const std::string& module_name) const;
  std::optional<pid_t> ChildPidOf(const std::string& module_name) const;
  bool ReapChildProcess(pid_t pid);

  void TransitionToForked(const std::string& module_name);
  void TransitionToIniting(const std::string& module_name);
  void TransitionToReady(const std::string& module_name);
  void TransitionToRunning(const std::string& module_name);
  void TransitionToFailed(const std::string& module_name, std::string failure_reason);
  std::optional<ModuleLifecycleRecord> LifecycleOf(const std::string& module_name) const;

  bool WaitForReadyOrTransitionFailed(
      const std::string& module_name,
      int parent_ready_pipe_fd,
      std::int64_t ready_timeout_ms);

  bool CanReleaseNextPriorityBatch(const std::vector<std::string>& current_batch_modules) const;

  RestartDecision HandleAbnormalChildExit(
      const std::string& module_name,
      const RestartPolicyConfig& policy_config,
      std::int64_t now_ms);
  bool IsMasterAlive() const;
  SupervisorObservabilitySnapshot ObservabilitySnapshot() const;

 private:
  mutable std::mt19937 rng_;
  std::unordered_map<std::string, pid_t> module_to_pid_;
  std::unordered_map<pid_t, std::string> pid_to_module_;
  std::unordered_map<std::string, ModuleLifecycleRecord> module_lifecycle_;
  RestartPolicy restart_policy_;
  bool master_alive_ = true;
  mutable SupervisorObservabilitySnapshot observability_;
};

}  // namespace mould::comm
