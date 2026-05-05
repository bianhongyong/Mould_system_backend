#include "launch_plan_config.hpp"
#include "ms_logging.hpp"
#include "test_helpers.hpp"

#include <gflags/gflags.h>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

DECLARE_int32(mould_test_startup_priority);

namespace {

std::filesystem::path UniqueTempDir() {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<std::uint64_t> dist;
  return std::filesystem::temp_directory_path() /
      ("mould_lp_fork_" + std::to_string(dist(gen)));
}

void WriteFile(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << content;
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  mould::InitApplicationLogging(argv[0]);

  const std::filesystem::path root = UniqueTempDir();
  std::filesystem::create_directories(root / "channels");

  const std::string io_json = R"({
  "input_channel": {
    "infer.results": {"slot_payload_bytes": "64"}
  },
  "output_channel": {
    "broker.frames": {"slot_payload_bytes": "8"}
  }
})";
  WriteFile(root / "channels" / "smoke_io.json", io_json);

  const std::string plan_json = std::string(R"({
  "modules": {
    "SmokeMod": {
      "module_name": "SmokeMod",
      "resource": { "mould_test_startup_priority": 77 },
      "module_params": {},
      "io_channels_config_path": "channels/smoke_io.json"
    }
  }
})");
  WriteFile(root / "launch_plan.txt", plan_json);

  const std::string launch_path = (root / "launch_plan.txt").string();
  mould::config::ParsedLaunchPlan plan;
  std::string error;
  if (!mould::config::ParseLaunchPlanFile(launch_path, &plan, &error)) {
    LOG(ERROR) << "parse failed: " << error;
    std::filesystem::remove_all(root);
    mould::ShutdownApplicationLogging();
    return 2;
  }
  if (!mould::config::ApplyLaunchPlanScalarsToRegisteredGflags(plan, &error)) {
    LOG(ERROR) << "gflags apply failed: " << error;
    std::filesystem::remove_all(root);
    mould::ShutdownApplicationLogging();
    return 3;
  }

  const pid_t child = fork();
  if (child < 0) {
    LOG(ERROR) << "fork failed";
    std::filesystem::remove_all(root);
    mould::ShutdownApplicationLogging();
    return 4;
  }
  if (child == 0) {
    const bool ok = FLAGS_mould_test_startup_priority == 77;
    _exit(ok ? 0 : 5);
  }

  int status = 0;
  if (waitpid(child, &status, 0) != child) {
    LOG(ERROR) << "waitpid failed";
    std::filesystem::remove_all(root);
    mould::ShutdownApplicationLogging();
    return 6;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    LOG(ERROR) << "child verification failed";
    std::filesystem::remove_all(root);
    mould::ShutdownApplicationLogging();
    return 7;
  }

  std::filesystem::remove_all(root);
  LOG(INFO) << "launch_plan_fork_smoke passed";
  mould::ShutdownApplicationLogging();
  return 0;
}
