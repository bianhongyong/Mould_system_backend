#include "restart_policy.hpp"

#include <algorithm>

namespace mould::comm {

namespace {

std::int64_t Pow2Saturating(std::int64_t exponent) {
  if (exponent <= 0) {
    return 1;
  }
  if (exponent >= 30) {
    return (1LL << 30);
  }
  return (1LL << exponent);
}

}  // namespace

RestartDecision RestartPolicy::EvaluateAbnormalExit(
    const std::string& module_name,
    const RestartPolicyConfig& config,
    std::int64_t now_ms) {
  RestartDecision decision;
  auto& state = module_state_[module_name];

  if (state.fuse_open_until_ms >= 0 && now_ms < state.fuse_open_until_ms) {
    decision.fuse_open = true;
    decision.should_restart = false;
    return decision;
  }

  const std::int64_t window_start = now_ms - std::max<std::int64_t>(0, config.restart_window_ms);
  while (!state.failure_timestamps_ms.empty() && state.failure_timestamps_ms.front() < window_start) {
    state.failure_timestamps_ms.pop_front();
  }
  state.failure_timestamps_ms.push_back(now_ms);
  decision.retries_in_window = static_cast<std::int64_t>(state.failure_timestamps_ms.size());

  if (decision.retries_in_window > std::max<std::int64_t>(0, config.restart_max_retries)) {
    decision.should_restart = false;
    decision.fuse_open = true;
    state.fuse_open_until_ms = now_ms + std::max<std::int64_t>(0, config.restart_fuse_ms);
    return decision;
  }

  decision.should_restart = true;
  const std::int64_t base_backoff = std::max<std::int64_t>(0, config.restart_backoff_ms);
  const std::int64_t exponent = std::max<std::int64_t>(0, decision.retries_in_window - 1);
  decision.restart_delay_ms = base_backoff * Pow2Saturating(exponent);
  return decision;
}

}  // namespace mould::comm
