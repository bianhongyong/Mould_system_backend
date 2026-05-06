#pragma once

#include "restart_policy.hpp"

#include <cstdint>
#include <string>

namespace mould::config {
struct ResourceSchema;
}  // namespace mould::config

namespace mould::comm {

/// Convert module name to process comm name (PR_SET_NAME limit: 15 bytes).
/// Prefixes with "M-" and truncates to 15 characters.
std::string ToProcessCommName(const std::string& module_name);

/// Convert config ResourceSchema to supervisor's RestartPolicyConfig.
RestartPolicyConfig ToPolicyConfig(const mould::config::ResourceSchema& schema);

}  // namespace mould::comm
