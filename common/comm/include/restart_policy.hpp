#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>

namespace mould::comm {

struct RestartPolicyConfig {
  std::int64_t restart_backoff_ms = 0;
  std::int64_t restart_max_retries = 0;
  std::int64_t restart_window_ms = 0;
  std::int64_t restart_fuse_ms = 0;
};

struct RestartDecision {
  bool should_restart = false;
  bool fuse_open = false;
  std::int64_t restart_delay_ms = 0;
  std::int64_t retries_in_window = 0;
};

class RestartPolicy {
 public:
  RestartDecision EvaluateAbnormalExit(
      const std::string& module_name,
      const RestartPolicyConfig& config,
      std::int64_t now_ms);

 private:
  struct ModulePolicyState {
    std::deque<std::int64_t> failure_timestamps_ms;
    std::int64_t fuse_open_until_ms = -1;
  };

  std::unordered_map<std::string, ModulePolicyState> module_state_;
};

}  // namespace mould::comm
