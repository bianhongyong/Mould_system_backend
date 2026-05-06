#include "channel_topology_config.hpp"
#include "launch_plan_config.hpp"
#include "log_pipe_manager.hpp"
#include "module_factory_registry.hpp"
#include "module_launcher.hpp"
#include "ms_logging.hpp"
#include "shm_bus_control_plane.hpp"
#include "subprocess_log_init.hpp"
#include "supervisor.hpp"

#include <gflags/gflags.h>
#include <poll.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

std::atomic<bool> g_shutdown_requested{false};

void HandleTerminationSignal(int) {
  g_shutdown_requested.store(true);
}

}  // namespace

int main(int argc, char** argv) {
  google::ParseCommandLineFlags(&argc, &argv, true);
  mould::InitApplicationLogging(argv[0]);

  const std::string launch_plan_path = mould::config::ResolveLaunchPlanPath(argc, argv);
  if (launch_plan_path.empty()) {
    LOG(ERROR) << "[backend_main] usage: " << argv[0] << " <launch_plan.json>";
    mould::ShutdownApplicationLogging();
    return 2;
  }
  LOG(INFO) << "[backend_main] starting with launch_plan_path=" << launch_plan_path;

  // Ignore SIGPIPE so write() to a broken pipe returns EPIPE instead of killing the process.
  ::signal(SIGPIPE, SIG_IGN);

  struct sigaction sa {};
  sa.sa_handler = HandleTerminationSignal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)sigaction(SIGINT, &sa, nullptr);
  (void)sigaction(SIGTERM, &sa, nullptr);
  LOG(INFO) << "[backend_main] signal handlers installed (SIGINT/SIGTERM, SIGPIPE ignored)";

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
  {
    std::string gflag_apply_error;
    if (!mould::config::ApplyLaunchPlanScalarsToMatchingRegisteredGflags(
            launch_plan, mould::config::LaunchPlanGflagMatchPolicy::kSkipUnknownKeys, &gflag_apply_error)) {
      LOG(ERROR) << "[backend_main] ApplyLaunchPlanScalarsToMatchingRegisteredGflags failed: "
                 << gflag_apply_error;
      mould::ShutdownApplicationLogging();
      return 12;
    }
    LOG(INFO) << "[backend_main] launch plan scalars applied to matching registered gflags";
  }

  // Build supervisor specs and module entry map.
  std::vector<mould::comm::SupervisorModuleSpec> supervisor_specs;
  supervisor_specs.reserve(launch_plan.modules.size());
  std::unordered_map<std::string, mould::config::ParsedModuleLaunchEntry> module_entries;
  for (const auto& module : launch_plan.modules) {
    supervisor_specs.push_back({module.module_name, module.resource_schema.startup_priority});
    module_entries.emplace(module.module_name, module);
  }

  // Set runtime environment variables.
  std::string env_error;
  std::vector<std::pair<std::string, std::string>> module_config_files;
  if (!mould::config::SetupRuntimeEnvironmentFromLaunchPlan(
          module_entries, launch_plan, &module_config_files, &env_error)) {
    LOG(ERROR) << "[backend_main] runtime env setup failed: " << env_error;
    mould::ShutdownApplicationLogging();
    return 8;
  }
  LOG(INFO) << "[backend_main] runtime env ready for fork model";

  // SHM control plane: create shared memory segments for all channels.
  mould::comm::ShmBusControlPlane control_plane;
  mould::comm::MiddlewareConfig middleware_config;
  middleware_config.shm_slot_count = launch_plan.communication_slot_count.value_or(256U);
  middleware_config.slot_payload_bytes = launch_plan.communication_slot_payload_bytes.value_or(1024U);
  if (!control_plane.ProvisionChannelTopologyFromModuleConfigs(
          module_config_files, &env_error, middleware_config)) {
    LOG(ERROR) << "[backend_main] control plane provisioning failed: " << env_error;
    mould::ShutdownApplicationLogging();
    return 9;
  }
  LOG(INFO) << "[backend_main] control plane provisioning success";

  // Build channel topology index for supervisor SHM hooks.
  mould::config::ChannelTopologyIndex supervisor_channel_topology;
  if (!mould::config::BuildChannelTopologyIndexFromFiles(
          module_config_files, &supervisor_channel_topology, &env_error)) {
    LOG(ERROR) << "[backend_main] failed to build channel topology for supervisor SHM hooks: " << env_error;
    mould::ShutdownApplicationLogging();
    return 10;
  }

  // Create supervisor and validate single-module-per-process invariant.
  mould::comm::Supervisor supervisor(static_cast<std::uint32_t>(std::random_device{}()));
  std::string supervisor_error;
  if (!supervisor.ValidateSingleModulePerProcessInvariant(supervisor_specs, &supervisor_error)) {
    LOG(ERROR) << "[backend_main] supervisor invariant failed: " << supervisor_error;
    mould::ShutdownApplicationLogging();
    return 4;
  }
  LOG(INFO) << "[backend_main] supervisor invariant validation passed";

  // ----- Unified Logging Pipeline Setup -----
  constexpr const char* kLogModuleName = "Logging_module";
  const bool has_log_module = module_entries.find(kLogModuleName) != module_entries.end();

  // Create log pipes for every module (and backend_main itself).
  std::unordered_map<std::string, mould::comm::ModulePipeSet> module_pipes;
  if (has_log_module) {
    for (const auto& [name, entry] : module_entries) {
      if (name == kLogModuleName) continue;
      mould::comm::ModulePipeSet ps;
      if (!mould::comm::CreateLogPipe(&ps, &supervisor_error)) {
        LOG(ERROR) << "[backend_main] failed to create log pipe for " << name << ": " << supervisor_error;
        mould::comm::CloseAllPipeFds(&module_pipes);
        mould::ShutdownApplicationLogging();
        return 11;
      }
      module_pipes[name] = ps;
    }
    // backend_main's own log pipe.
    {
      mould::comm::ModulePipeSet ps;
      if (!mould::comm::CreateLogPipe(&ps, &supervisor_error)) {
        LOG(ERROR) << "[backend_main] failed to create log pipe for backend_main: " << supervisor_error;
        mould::comm::CloseAllPipeFds(&module_pipes);
        mould::ShutdownApplicationLogging();
        return 11;
      }
      module_pipes["backend_main"] = ps;
    }
  }
  LOG(INFO) << "[backend_main] log pipes created count=" << module_pipes.size();

  // Fork the log module (if present) and wait for its ready signal.
  pid_t log_module_pid = 0;
  if (has_log_module) {
    int ready_event_fd = ::eventfd(0, EFD_CLOEXEC);
    if (ready_event_fd < 0) {
      LOG(ERROR) << "[backend_main] eventfd failed errno=" << errno;
      mould::comm::CloseAllPipeFds(&module_pipes);
      mould::ShutdownApplicationLogging();
      return 11;
    }

    auto log_entry_it = module_entries.find(kLogModuleName);
    log_module_pid = mould::comm::ForkLogModule(
        log_entry_it->second, &supervisor, ready_event_fd, module_pipes, &supervisor_error);
    if (log_module_pid == 0) {
      LOG(ERROR) << "[backend_main] failed to fork log module: " << supervisor_error;
      ::close(ready_event_fd);
      mould::comm::CloseAllPipeFds(&module_pipes);
      mould::ShutdownApplicationLogging();
      return 11;
    }
    LOG(INFO) << "[backend_main] log module forked pid=" << log_module_pid;

    // Wait for log module ready signal via eventfd.
    std::uint64_t ready_val = 0;
    constexpr int kLogReadyTimeoutMs = 5000;
    struct pollfd pfd;
    pfd.fd = ready_event_fd;
    pfd.events = POLLIN;
    int poll_ret = ::poll(&pfd, 1, kLogReadyTimeoutMs);
    if (poll_ret <= 0) {
      LOG(ERROR) << "[backend_main] log module ready timeout";
      ::close(ready_event_fd);
      kill(log_module_pid, SIGTERM);
      waitpid(log_module_pid, nullptr, 0);
      supervisor.ReapChildProcess(log_module_pid);
      mould::comm::CloseAllPipeFds(&module_pipes);
      mould::ShutdownApplicationLogging();
      return 11;
    }
    ssize_t n = ::read(ready_event_fd, &ready_val, sizeof(ready_val));
    ::close(ready_event_fd);
    if (n != sizeof(ready_val) || ready_val != 1) {
      LOG(ERROR) << "[backend_main] log module ready signal invalid";
      mould::comm::CloseAllPipeFds(&module_pipes);
      mould::ShutdownApplicationLogging();
      return 11;
    }
    LOG(INFO) << "[backend_main] log module ready, starting business modules";

    // Redirect backend_main's own stderr to its log pipe.
    auto main_pipe_it = module_pipes.find("backend_main");
    if (main_pipe_it != module_pipes.end() && main_pipe_it->second.write_fd >= 0) {
      mould::RedirectStderrToLogPipe(main_pipe_it->second.write_fd);
      main_pipe_it->second.write_fd = -1;
    }
    LOG(INFO) << "[backend_main] backend_main stderr redirected to log pipe";
  }

  // ----- Launch Business Modules in Priority Batches -----
  std::vector<mould::comm::SupervisorModuleSpec> business_specs;
  std::unordered_map<std::string, mould::config::ParsedModuleLaunchEntry> business_entries;
  for (const auto& spec : supervisor_specs) {
    if (spec.module_name == kLogModuleName) continue;
    business_specs.push_back(spec);
    business_entries[spec.module_name] = module_entries.at(spec.module_name);
  }

  const auto startup_batches = supervisor.BuildInitialStartupBatches(business_specs);
  LOG(INFO) << "[backend_main] startup priority batches count=" << startup_batches.size();
  for (const auto& batch : startup_batches) {
    std::vector<std::string> batch_modules;
    LOG(INFO) << "[backend_main] launching startup batch, size=" << batch.size();
    for (const auto& module_spec : batch) {
      batch_modules.push_back(module_spec.module_name);
      const auto it = business_entries.find(module_spec.module_name);
      if (it == business_entries.end()) {
        LOG(ERROR) << "[backend_main] module entry missing for " << module_spec.module_name;
        mould::ShutdownApplicationLogging();
        return 5;
      }
      LOG(INFO) << "[backend_main] launching module=" << module_spec.module_name
                << " priority=" << module_spec.startup_priority;
      if (!mould::comm::LaunchOneModule(it->second, &supervisor,
                                        has_log_module ? &module_pipes : nullptr,
                                        &supervisor_error)) {
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

  // ----- Monitor Loop -----
  LOG(INFO) << "[backend_main] entering monitor loop";

  mould::comm::Supervisor::MonitorDeps monitor_deps;
  monitor_deps.shutdown_requested = &g_shutdown_requested;
  monitor_deps.module_entries = &module_entries;
  monitor_deps.take_consumers_offline =
      [&](std::uint32_t dead_pid) -> std::uint32_t {
        return control_plane.TakeAllConsumersOfflineForProcessPid(
            dead_pid, supervisor_channel_topology, middleware_config.slot_payload_bytes);
      };

  // Business module restart: look up entry and re-launch.
  monitor_deps.restart_business_module =
      [&](const std::string& module_name, std::string* error) -> bool {
        auto it = module_entries.find(module_name);
        if (it == module_entries.end()) {
          if (error != nullptr) {
            *error = "module entry not found: " + module_name;
          }
          return false;
        }
        return mould::comm::LaunchOneModule(
            it->second, &supervisor,
            has_log_module ? &module_pipes : nullptr, error);
      };

  // Log module restart: reuse existing pipes, re-fork the log module.
  monitor_deps.has_log_module = has_log_module;
  monitor_deps.mutable_log_module_pid = &log_module_pid;
  monitor_deps.restart_log_module =
      [&](std::string* error) -> bool {
        int ready_event_fd = ::eventfd(0, EFD_CLOEXEC);
        if (ready_event_fd < 0) {
          if (error != nullptr) {
            *error = "eventfd failed";
          }
          return false;
        }
        auto log_entry_it = module_entries.find(kLogModuleName);
        pid_t new_pid = mould::comm::ForkLogModule(
            log_entry_it->second, &supervisor, ready_event_fd, module_pipes, error);
        if (new_pid == 0) {
          ::close(ready_event_fd);
          return false;
        }

        // Wait for ready.
        std::uint64_t ready_val = 0;
        struct pollfd pfd;
        pfd.fd = ready_event_fd;
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, 5000) > 0) {
          [[maybe_unused]] ssize_t unused = ::read(ready_event_fd, &ready_val, sizeof(ready_val));
          (void)unused;
        }
        ::close(ready_event_fd);
        log_module_pid = new_pid;
        LOG(INFO) << "[backend_main] log module restarted pid=" << log_module_pid;
        return true;
      };

  // Close a business module's log pipe when its fuse opens.
  monitor_deps.close_log_pipe =
      [&](const std::string& module_name) {
        auto pipe_it = module_pipes.find(module_name);
        if (pipe_it != module_pipes.end() && pipe_it->second.write_fd >= 0) {
          ::close(pipe_it->second.write_fd);
          pipe_it->second.write_fd = -1;
        }
      };

  supervisor.RunMonitorLoop(monitor_deps);

  // ----- Graceful Shutdown -----
  LOG(INFO) << "[backend_main] shutdown requested, starting graceful cleanup";
  for (const auto& [module_name, _] : module_entries) {
    (void)_;
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
    (void)control_plane.TakeAllConsumersOfflineForProcessPid(
        static_cast<std::uint32_t>(pid),
        supervisor_channel_topology,
        middleware_config.slot_payload_bytes);
    (void)supervisor.ReapChildProcess(pid);
  }

  for (const auto& [module_name, _] : module_entries) {
    (void)_;
    if (auto pid = supervisor.ChildPidOf(module_name); pid.has_value()) {
      LOG(WARNING) << "[backend_main] sending SIGKILL to lingering module=" << module_name
                   << " pid=" << *pid;
      kill(*pid, SIGKILL);
      (void)waitpid(*pid, nullptr, 0);
      (void)control_plane.TakeAllConsumersOfflineForProcessPid(
          static_cast<std::uint32_t>(*pid),
          supervisor_channel_topology,
          middleware_config.slot_payload_bytes);
      (void)supervisor.ReapChildProcess(*pid);
    }
  }

  LOG(INFO) << "[backend_main] exit normally";
  mould::comm::CloseAllPipeFds(&module_pipes);
  control_plane.FinalizeUnlinkManagedSegments();
  mould::ShutdownApplicationLogging();
  return 0;
}
