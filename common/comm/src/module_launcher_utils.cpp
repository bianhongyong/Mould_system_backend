#include "module_launcher_utils.hpp"
#include "launch_plan_config.hpp"

#include <string>

namespace mould::comm {

std::string ToProcessCommName(const std::string& module_name) {
  constexpr std::size_t kMaxCommLen = 15;  // PR_SET_NAME limit without trailing '\0'
  const std::string prefixed_name = "M-" + module_name;
  if (prefixed_name.size() <= kMaxCommLen) {
    return prefixed_name;
  }
  return prefixed_name.substr(0, kMaxCommLen);
}

RestartPolicyConfig ToPolicyConfig(const mould::config::ResourceSchema& schema) {
  return RestartPolicyConfig{
      .restart_backoff_ms = schema.restart_backoff_ms,
      .restart_max_retries = schema.restart_max_retries,
      .restart_window_ms = schema.restart_window_ms,
      .restart_fuse_ms = schema.restart_fuse_ms,
  };
}

}  // namespace mould::comm
