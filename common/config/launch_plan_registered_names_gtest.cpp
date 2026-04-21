#include "launch_plan_config.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <unordered_set>

namespace mould::config {
namespace {

std::filesystem::path MakeTempRoot() {
  std::random_device rd;
  std::mt19937_64 rng(rd());
  return std::filesystem::temp_directory_path() / ("mould_lp_reg_" + std::to_string(rng()));
}

void WriteTextFile(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << content;
}

TEST(LaunchPlanConfigGtest, LaunchPlanParser_ValidateAgainstRegisteredNames_FailsOnMismatch) {
  const auto root = MakeTempRoot();
  WriteTextFile(root / "io.json", R"({
    "input_channel": {},
    "output_channel": {}
  })");
  WriteTextFile(root / "launch_plan.json", R"({
    "modules": {
      "UnknownMod": {
        "module_name": "UnknownMod",
        "resource": {
          "startup_priority": 1,
          "cpu_set": "0",
          "restart_backoff_ms": 10,
          "restart_max_retries": 1,
          "restart_window_ms": 100,
          "restart_fuse_ms": 1000,
          "ready_timeout_ms": 100
        },
        "module_params": {},
        "io_channels_config_path": "io.json"
      }
    }
  })");

  ParsedLaunchPlan out;
  std::string error;
  const std::unordered_set<std::string> registered = {"KnownMod"};
  LaunchPlanValidationOptions options;
  options.enforce_strict_resource_schema = true;
  options.registered_module_names = &registered;
  const bool ok = ParseLaunchPlanFile((root / "launch_plan.json").string(), &out, &error, options);
  std::filesystem::remove_all(root);

  EXPECT_FALSE(ok);
  EXPECT_NE(error.find("unregistered module factory"), std::string::npos);
}

}  // namespace
}  // namespace mould::config
