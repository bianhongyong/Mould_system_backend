#include "launch_plan_config.hpp"
#include "ms_logging.hpp"
#include "module_factory_registry.hpp"
#include "ready_pipe_protocol.hpp"
#include "shm_bus_control_plane.hpp"
#include "supervisor.hpp"

#include <signal.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

std::atomic<bool> g_shutdown_requested{false};

void HandleTerminationSignal(int) {
  g_shutdown_requested.store(true);
}

std::int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string ToProcessCommName(const std::string& module_name) {
  constexpr std::size_t kMaxCommLen = 15;  // PR_SET_NAME limit without trailing '\0'
  const std::string prefixed_name = "M-" + module_name;
  if (prefixed_name.size() <= kMaxCommLen) {
    return prefixed_name;
  }
  return prefixed_name.substr(0, kMaxCommLen);
}

mould::comm::RestartPolicyConfig ToPolicyConfig(const mould::config::ResourceSchema& schema) {
  return mould::comm::RestartPolicyConfig{
      .restart_backoff_ms = schema.restart_backoff_ms,
      .restart_max_retries = schema.restart_max_retries,
      .restart_window_ms = schema.restart_window_ms,
      .restart_fuse_ms = schema.restart_fuse_ms,
  };
}

bool LaunchOneModule(
    const mould::config::ParsedModuleLaunchEntry& module_entry,
    mould::comm::Supervisor* supervisor,
    std::unordered_map<pid_t, std::string>* pid_to_module,
    std::string* out_error) {
  int parent_read_fd = -1;
  int child_write_fd = -1;
  if (!mould::comm::ReadyPipeProtocol::CreateParentManagedPipe(&parent_read_fd, &child_write_fd, out_error)) {
    return false;
  }

  const std::string module_name = module_entry.module_name;
  const std::string io_config_path = module_entry.io_channels_config_path_resolved;
  const std::string cpu_set = module_entry.resource_schema.cpu_set;
  const bool fork_ok =
      supervisor->ForkModuleProcess(module_name, [module_name, io_config_path, cpu_set, child_write_fd]() mutable {
    const std::string comm_name = ToProcessCommName(module_name);
    if (prctl(PR_SET_NAME, comm_name.c_str(), 0, 0, 0) != 0) {
      LOG(WARNING) << "[backend_main] failed to set process comm name for module=" << module_name
                   << " comm_name=" << comm_name << " errno=" << errno;
    }
    if (!cpu_set.empty()) {
      try {
        std::stringstream ss(cpu_set);
        std::string token;
        std::vector<int> cpus;
        cpus.reserve(8);
        int max_cpu = -1;
        while (std::getline(ss, token, ',')) {
          const int cpu = std::stoi(token);
          cpus.push_back(cpu);
          if (cpu > max_cpu) {
            max_cpu = cpu;
          }
        }
        if (!cpus.empty()) {
          cpu_set_t* affinity_set = CPU_ALLOC(static_cast<std::size_t>(max_cpu) + 1U);
          if (affinity_set == nullptr) {
            LOG(ERROR) << "[backend_main] CPU_ALLOC failed for module=" << module_name
                       << " cpu_set=" << cpu_set;
            mould::comm::ReadyPipeProtocol::CloseFdIfOpen(&child_write_fd);
            return 5;
          }
          const std::size_t affinity_set_size = CPU_ALLOC_SIZE(static_cast<std::size_t>(max_cpu) + 1U);
          CPU_ZERO_S(affinity_set_size, affinity_set);
          for (const int cpu : cpus) {
            CPU_SET_S(cpu, affinity_set_size, affinity_set);
          }
          const int rc = sched_setaffinity(0, affinity_set_size, affinity_set);
          CPU_FREE(affinity_set);
          if (rc != 0) {
            LOG(ERROR) << "[backend_main] sched_setaffinity failed for module=" << module_name
                       << " cpu_set=" << cpu_set << " errno=" << errno;
            mould::comm::ReadyPipeProtocol::CloseFdIfOpen(&child_write_fd);
            return 5;
          }
        }
      } catch (const std::exception& ex) {
        LOG(ERROR) << "[backend_main] failed to parse cpu_set for module=" << module_name
                   << " cpu_set=" << cpu_set << " what=" << ex.what();
        mould::comm::ReadyPipeProtocol::CloseFdIfOpen(&child_write_fd);
        return 5;
      }
    }
    std::string error;
    mould::comm::ModuleFactoryConfig config;
    config.module_name = module_name;
    config.runtime_context.config.module_channel_config_path = io_config_path;
    auto module = mould::comm::ModuleFactoryRegistry::Instance().Create(module_name, config, &error);
    if (!module) {
      mould::comm::ReadyPipeProtocol::CloseFdIfOpen(&child_write_fd);
      return 2;
    }
    if (!mould::comm::ReadyPipeProtocol::SendReady(child_write_fd, &error)) {
      mould::comm::ReadyPipeProtocol::CloseFdIfOpen(&child_write_fd);
      return 3;
    }
    mould::comm::ReadyPipeProtocol::CloseFdIfOpen(&child_write_fd);
    return module->Run() ? 0 : 4;
  }, out_error);
  mould::comm::ReadyPipeProtocol::CloseFdIfOpen(&child_write_fd);
  if (!fork_ok) {
    mould::comm::ReadyPipeProtocol::CloseFdIfOpen(&parent_read_fd);
    return false;
  }

  const bool ready = supervisor->WaitForReadyOrTransitionFailed(
      module_name, parent_read_fd, module_entry.resource_schema.ready_timeout_ms);
  mould::comm::ReadyPipeProtocol::CloseFdIfOpen(&parent_read_fd);
  if (!ready) {
    if (auto pid = supervisor->ChildPidOf(module_name); pid.has_value()) {
      kill(*pid, SIGTERM);
      (void)waitpid(*pid, nullptr, 0);
      (void)supervisor->ReapChildProcess(*pid);
    }
    if (out_error != nullptr) {
      *out_error = "module failed READY barrier: " + module_name;
    }
    return false;
  }
  if (auto pid = supervisor->ChildPidOf(module_name); pid.has_value()) {
    (*pid_to_module)[*pid] = module_name;
  }
  return true;
}

std::string ResolveLaunchPlanPath(int argc, char** argv) {
  if (argc > 1 && argv[1] != nullptr && std::strlen(argv[1]) > 0) {
    return argv[1];
  }
  namespace fs = std::filesystem;
  const std::vector<fs::path> candidates = {
      "launch_plan.json",
      "../launch_plan.json",
      "../../launch_plan.json",
      "../../../launch_plan.json",
      "../../../../launch_plan.json",
  };
  for (const auto& candidate : candidates) {
    std::error_code ec;
    if (fs::exists(candidate, ec) && !ec) {
      return candidate.lexically_normal().string();
    }
  }
  // Keep original default path for clear error output.
  return "launch_plan.json";
}

bool SetupRuntimeEnvironmentFromLaunchPlan(
    const std::unordered_map<std::string, mould::config::ParsedModuleLaunchEntry>& module_entries,
    const mould::config::ParsedLaunchPlan& launch_plan,
    std::vector<std::pair<std::string, std::string>>* out_module_config_files,
    std::string* out_error) {
  if (out_module_config_files == nullptr) {
    if (out_error != nullptr) {
      *out_error = "out_module_config_files is null";
    }
    return false;
  }
  out_module_config_files->clear();
  out_module_config_files->reserve(module_entries.size());
  std::ostringstream module_configs;
  bool first = true;
  for (const auto& [module_name, module_entry] : module_entries) {
    if (module_entry.io_channels_config_path_resolved.empty()) {
      if (out_error != nullptr) {
        *out_error = "module '" + module_name + "' has empty resolved io config path";
      }
      return false;
    }
    if (!first) {
      module_configs << ";";
    }
    first = false;
    module_configs << module_name << "=" << module_entry.io_channels_config_path_resolved;
    out_module_config_files->push_back({module_name, module_entry.io_channels_config_path_resolved});
  }

  const std::string configs_value = module_configs.str();
  if (configs_value.empty()) {
    if (out_error != nullptr) {
      *out_error = "no module configs found when preparing MOULD_MODULE_CHANNEL_CONFIGS";
    }
    return false;
  }

  if (setenv("MOULD_MODULE_CHANNEL_CONFIGS", configs_value.c_str(), 1) != 0) {
    if (out_error != nullptr) {
      *out_error = "setenv MOULD_MODULE_CHANNEL_CONFIGS failed, errno=" + std::to_string(errno);
    }
    return false;
  }
  if (setenv("MOULD_FORK_INHERITANCE_TOKEN", "backend_main_fork_model", 1) != 0) {
    if (out_error != nullptr) {
      *out_error = "setenv MOULD_FORK_INHERITANCE_TOKEN failed, errno=" + std::to_string(errno);
    }
    return false;
  }
  const std::uint32_t configured_slot_count = launch_plan.communication_slot_count.value_or(256U);
  const std::string slot_count_value = std::to_string(configured_slot_count);
  if (setenv("MOULD_SHM_SLOT_COUNT", slot_count_value.c_str(), 1) != 0) {
    if (out_error != nullptr) {
      *out_error = "setenv MOULD_SHM_SLOT_COUNT failed, errno=" + std::to_string(errno);
    }
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  mould::InitApplicationLogging(argv[0]);
  const std::string launch_plan_path = ResolveLaunchPlanPath(argc, argv);
  if (launch_plan_path.empty()) {
    LOG(ERROR) << "[backend_main] launch plan path is empty";
    mould::ShutdownApplicationLogging();
    return 2;
  }
  LOG(INFO) << "[backend_main] starting with launch_plan_path=" << launch_plan_path;

  struct sigaction sa {};
  sa.sa_handler = HandleTerminationSignal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)sigaction(SIGINT, &sa, nullptr);
  (void)sigaction(SIGTERM, &sa, nullptr);
  LOG(INFO) << "[backend_main] signal handlers installed (SIGINT/SIGTERM)";

  const auto registered_names = mould::comm::ModuleFactoryRegistry::Instance().RegisteredNames();
  LOG(INFO) << "[backend_main] registered module factories count=" << registered_names.size();
  mould::config::LaunchPlanValidationOptions options;
  options.enforce_strict_resource_schema = true;
  options.registered_module_names = &registered_names;

  mould::config::ParsedLaunchPlan launch_plan;
  std::string parse_error;
  if (!mould::config::ParseLaunchPlanFile(launch_plan_path, &launch_plan, &parse_error, options)) {
    LOG(ERROR) << "[backend_main] failed to parse launch plan '" << launch_plan_path
               << "': " << parse_error;
    mould::ShutdownApplicationLogging();
    return 3;
  }
  LOG(INFO) << "[backend_main] launch plan parsed, modules count=" << launch_plan.modules.size();
  if (launch_plan.minloglevel.has_value()) {
    FLAGS_minloglevel = static_cast<int>(*(launch_plan.minloglevel));
    LOG(INFO) << "[backend_main] applied launch plan minloglevel=" << FLAGS_minloglevel;
  }

  std::vector<mould::comm::SupervisorModuleSpec> supervisor_specs;
  supervisor_specs.reserve(launch_plan.modules.size());
  std::unordered_map<std::string, mould::config::ParsedModuleLaunchEntry> module_entries;
  for (const auto& module : launch_plan.modules) {
    supervisor_specs.push_back({module.module_name, module.resource_schema.startup_priority});
    module_entries.emplace(module.module_name, module);
  }
  std::string env_error;
  std::vector<std::pair<std::string, std::string>> module_config_files;
  if (!SetupRuntimeEnvironmentFromLaunchPlan(module_entries, launch_plan, &module_config_files, &env_error)) {
    LOG(ERROR) << "[backend_main] runtime env setup failed: " << env_error;
    mould::ShutdownApplicationLogging();
    return 8;
  }
  LOG(INFO) << "[backend_main] runtime env ready for fork model";
  mould::comm::ShmBusControlPlane control_plane;
  mould::comm::MiddlewareConfig middleware_config;
  middleware_config.shm_slot_count = launch_plan.communication_slot_count.value_or(256U);
  if (!control_plane.ProvisionChannelTopologyFromModuleConfigs(
          module_config_files, &env_error, middleware_config)) {
    LOG(ERROR) << "[backend_main] control plane provisioning failed: " << env_error;
    mould::ShutdownApplicationLogging();
    return 9;
  }
  LOG(INFO) << "[backend_main] control plane provisioning success";

  mould::comm::Supervisor supervisor(static_cast<std::uint32_t>(std::random_device{}()));
  std::string supervisor_error;
  if (!supervisor.ValidateSingleModulePerProcessInvariant(supervisor_specs, &supervisor_error)) {
    LOG(ERROR) << "[backend_main] supervisor invariant failed: " << supervisor_error;
    mould::ShutdownApplicationLogging();
    return 4;
  }
  LOG(INFO) << "[backend_main] supervisor invariant validation passed";

  std::unordered_map<pid_t, std::string> pid_to_module;
  const auto startup_batches = supervisor.BuildInitialStartupBatches(supervisor_specs);
  LOG(INFO) << "[backend_main] startup priority batches count=" << startup_batches.size();
  for (const auto& batch : startup_batches) {
    std::vector<std::string> batch_modules;
    LOG(INFO) << "[backend_main] launching startup batch, size=" << batch.size();
    for (const auto& module_spec : batch) {
      batch_modules.push_back(module_spec.module_name);
      const auto it = module_entries.find(module_spec.module_name);
      if (it == module_entries.end()) {
        LOG(ERROR) << "[backend_main] module entry missing for " << module_spec.module_name;
        mould::ShutdownApplicationLogging();
        return 5;
      }
      LOG(INFO) << "[backend_main] launching module=" << module_spec.module_name
                << " priority=" << module_spec.startup_priority;
      if (!LaunchOneModule(it->second, &supervisor, &pid_to_module, &supervisor_error)) {
        LOG(ERROR) << "[backend_main] failed to launch module " << module_spec.module_name << ": "
                   << supervisor_error;
        mould::ShutdownApplicationLogging();
        return 6;
      }
      LOG(INFO) << "[backend_main] module launch success=" << module_spec.module_name;
    }
    if (!supervisor.CanReleaseNextPriorityBatch(batch_modules)) {
      LOG(ERROR) << "[backend_main] startup barrier check failed for current priority batch";
      mould::ShutdownApplicationLogging();
      return 7;
    }
    LOG(INFO) << "[backend_main] startup barrier passed for batch";
  }
  LOG(INFO) << "[backend_main] entering monitor loop";

  while (!g_shutdown_requested.load()) {
    int status = 0;
    const pid_t pid = waitpid(-1, &status, WNOHANG);
    if (pid == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    if (pid < 0) {
      if (errno == EINTR) {
        continue;
      }
      LOG(WARNING) << "[backend_main] waitpid returned error errno=" << errno
                   << ", leaving monitor loop";
      break;
    }

    const auto module_it = pid_to_module.find(pid);
    if (module_it == pid_to_module.end()) {
      LOG(WARNING) << "[backend_main] received child exit for unmanaged pid=" << pid;
      continue;
    }
    const std::string module_name = module_it->second;
    pid_to_module.erase(module_it);
    (void)supervisor.ReapChildProcess(pid);
    LOG(ERROR ) << "[backend_main] child exited module=" << module_name << " pid=" << pid
                 << " raw_status=" << status;

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
      LOG(INFO) << "[backend_main] module exited cleanly, no restart module=" << module_name;
      continue;
    }

    const auto entry_it = module_entries.find(module_name);
    if (entry_it == module_entries.end()) {
      continue;
    }

    const mould::comm::RestartDecision decision = supervisor.HandleAbnormalChildExit(
        module_name, ToPolicyConfig(entry_it->second.resource_schema), NowMs());
    LOG(INFO) << "[backend_main] restart decision module=" << module_name
              << " should_restart=" << decision.should_restart
              << " fuse_open=" << decision.fuse_open
              << " delay_ms=" << decision.restart_delay_ms;
    if (decision.should_restart) {
      if (decision.restart_delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(decision.restart_delay_ms));
      }
      std::string restart_error;
      if (!LaunchOneModule(entry_it->second, &supervisor, &pid_to_module, &restart_error)) {
        LOG(ERROR) << "[backend_main] restart launch failed module=" << module_name
                   << " error=" << restart_error;
      } else {
        LOG(INFO) << "[backend_main] restart launch success module=" << module_name;
      }
    }
  }
  LOG(INFO) << "[backend_main] shutdown requested, starting graceful cleanup";

  for (const auto& [module_name, _] : module_entries) {
    (void)module_name;
    if (auto pid = supervisor.ChildPidOf(module_name); pid.has_value()) {
      LOG(INFO) << "[backend_main] sending SIGTERM to module=" << module_name << " pid=" << *pid;
      kill(*pid, SIGTERM);
    }
  }

  const auto shutdown_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < shutdown_deadline) {
    int status = 0;
    const pid_t pid = waitpid(-1, &status, WNOHANG);
    if (pid <= 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    LOG(INFO) << "[backend_main] reaped pid during graceful window pid=" << pid;
    (void)supervisor.ReapChildProcess(pid);
  }

  for (const auto& [module_name, _] : module_entries) {
    (void)_;
    if (auto pid = supervisor.ChildPidOf(module_name); pid.has_value()) {
      LOG(WARNING) << "[backend_main] sending SIGKILL to lingering module=" << module_name
                   << " pid=" << *pid;
      kill(*pid, SIGKILL);
      (void)waitpid(*pid, nullptr, 0);
      (void)supervisor.ReapChildProcess(*pid);
    }
  }

  LOG(INFO) << "[backend_main] exit normally";
  control_plane.FinalizeUnlinkManagedSegments();
  mould::ShutdownApplicationLogging();
  return 0;
}
