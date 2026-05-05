#pragma once

#include "channel_topology_config.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace mould::config {

// Launch plan (JSON in `launch_plan.txt` or any path): top-level object MUST contain `modules`
// and MAY contain `minloglevel` (integer in [0,3]). `modules` is an object whose keys are
// module ids. Each module entry MUST include:
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

struct ResourceSchema {
  std::int64_t startup_priority = 0;
  std::string cpu_set;
  std::int64_t restart_backoff_ms = 0;
  std::int64_t restart_max_retries = 0;
  std::int64_t restart_window_ms = 0;
  std::int64_t restart_fuse_ms = 0;
  std::int64_t ready_timeout_ms = 0;
};

class ResourceSchemaValidator {
 public:
  // Validates required fields and normalizes legacy cpu_id into cpu_set when cpu_set is absent.
  // Returns false with field-path-rich error on validation failure.
  static bool ValidateAndNormalize(
      std::unordered_map<std::string, LaunchPlanScalar>* resource,
      ResourceSchema* out_schema,
      std::string* out_error,
      const std::string& field_prefix);
};

struct LaunchPlanValidationOptions {
  bool enforce_strict_resource_schema = false;
  const std::unordered_set<std::string>* registered_module_names = nullptr;
};

struct ParsedModuleLaunchEntry {
  // Key of this module under `modules` in launch_plan.json (must equal `module_name`).
  std::string modules_dict_key;
  std::string module_name;
  std::unordered_map<std::string, LaunchPlanScalar> resource;
  ResourceSchema resource_schema;
  std::unordered_map<std::string, LaunchPlanScalar> module_params;
  std::string io_channels_config_path_resolved;
  ModuleChannelConfig channels;
};

struct ParsedLaunchPlan {
  std::filesystem::path launch_plan_path;
  std::optional<std::int64_t> minloglevel;
  std::optional<std::uint32_t> communication_slot_count;
  std::optional<std::size_t> communication_slot_payload_bytes;
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
  std::string* out_error,
  const LaunchPlanValidationOptions& options = {});

// After ParseLaunchPlanFile: for every key in every module's `resource` and `module_params`,
// calls `google::SetCommandLineOption` with the same key name. Unknown keys fail (task 2.3).
// Intended for the fork test binary and unit tests; not used by production daemon code.
bool ApplyLaunchPlanScalarsToRegisteredGflags(
    const ParsedLaunchPlan& plan,
    std::string* out_error);

}  // namespace mould::config
