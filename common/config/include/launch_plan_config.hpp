#pragma once

#include "channel_topology_config.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace mould::config {

// Launch plan (JSON in `launch_plan.txt` or any path): top-level object MUST contain exactly
// `modules`, an object whose keys are module ids. Each module entry MUST include:
//   `module_name` (string, MUST equal the parent key in `modules`),
//   `resource` (object of scalars), `module_params` (object of scalars),
//   `io_channels_config_path` (string path to that module's I/O JSON file).
// Module I/O JSON: object with `input_channel` and `output_channel` only; each side is an
// object mapping channel name -> parameter object (string values in the topology layer).
// Relative `io_channels_config_path` values are resolved against the parent directory of the
// launch plan file passed to ParseLaunchPlanFile.
//
// Multi-process / gflags (integration contract): call ParseLaunchPlanFile in the parent
// process first; then ApplyLaunchPlanScalarsToRegisteredGflags (or equivalent per-key
// SetCommandLineOption) before fork(2). Child processes inherit the updated flag values.
// Re-parsing in children is optional if the same launch plan path remains valid; tests cover
// the parent-first assignment order.

// Scalar values parsed from JSON in `resource` / `module_params` (task 2.2).
using LaunchPlanScalar = std::variant<std::string, std::int64_t, double, bool>;

struct ParsedModuleLaunchEntry {
  // Key of this module under `modules` in launch_plan.json (must equal `module_name`).
  std::string modules_dict_key;
  std::string module_name;
  std::unordered_map<std::string, LaunchPlanScalar> resource;
  std::unordered_map<std::string, LaunchPlanScalar> module_params;
  ModuleChannelConfig channels;
};

struct ParsedLaunchPlan {
  std::filesystem::path launch_plan_path;
  std::vector<ParsedModuleLaunchEntry> modules;
  ChannelTopologyIndex global_topology;
};

// Opens `launch_plan_file_path` (full path to the launch plan file). Resolves each
// `io_channels_config_path` relative to the launch plan's parent directory when not absolute.
// On success fills `out_plan` and returns true. On failure returns false and sets `out_error`
// (includes module id and field paths where applicable; see unit tests).
bool ParseLaunchPlanFile(
    const std::string& launch_plan_file_path,
    ParsedLaunchPlan* out_plan,
    std::string* out_error);

// After ParseLaunchPlanFile: for every key in every module's `resource` and `module_params`,
// calls `google::SetCommandLineOption` with the same key name. Unknown keys fail (task 2.3).
// Intended for the fork test binary and unit tests; not used by production daemon code.
bool ApplyLaunchPlanScalarsToRegisteredGflags(
    const ParsedLaunchPlan& plan,
    std::string* out_error);

}  // namespace mould::config
