#include "module_launcher.hpp"
#include "launch_plan_config.hpp"
#include "log_pipe_manager.hpp"
#include "module_factory_registry.hpp"
#include "module_launcher_utils.hpp"
#include "ready_pipe_protocol.hpp"
#include "supervisor.hpp"
#include "subprocess_log_init.hpp"
#include "logging_fork_gflags.hpp"

#include <glog/logging.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mould::comm {

bool LaunchOneModule(
    const mould::config::ParsedModuleLaunchEntry& module_entry,
    Supervisor* supervisor,
    const std::unordered_map<std::string, ModulePipeSet>* all_module_pipes,
    std::string* out_error) {
  int parent_read_fd = -1;
  int child_write_fd = -1;
  if (!ReadyPipeProtocol::CreateParentManagedPipe(&parent_read_fd, &child_write_fd, out_error)) {
    return false;
  }

  const std::string module_name = module_entry.module_name;
  const std::string io_config_path = module_entry.io_channels_config_path_resolved;
  const std::string cpu_set = module_entry.resource_schema.cpu_set;

  // Determine this module's log pipe write fd.
  int log_pipe_write_fd = -1;
  if (all_module_pipes != nullptr) {
    auto it = all_module_pipes->find(module_name);
    if (it != all_module_pipes->end()) {
      log_pipe_write_fd = it->second.write_fd;
    }
  }

  const bool fork_ok =
      supervisor->ForkModuleProcess(module_name, [module_name, io_config_path, cpu_set,
                                                   child_write_fd, log_pipe_write_fd,
                                                   all_module_pipes]() mutable {
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
            ReadyPipeProtocol::CloseFdIfOpen(&child_write_fd);
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
            ReadyPipeProtocol::CloseFdIfOpen(&child_write_fd);
            return 5;
          }
        }
      } catch (const std::exception& ex) {
        LOG(ERROR) << "[backend_main] failed to parse cpu_set for module=" << module_name
                   << " cpu_set=" << cpu_set << " what=" << ex.what();
        ReadyPipeProtocol::CloseFdIfOpen(&child_write_fd);
        return 5;
      }
    }

    // Redirect stderr to log pipe if available.
    if (log_pipe_write_fd >= 0) {
      RedirectStderrToLogPipe(log_pipe_write_fd);
    }

    // Close other modules' write_fds to avoid fd leaks.
    if (all_module_pipes != nullptr) {
      CloseOtherWriteFds(module_name, *all_module_pipes);
    }

    std::string error;
    ModuleFactoryConfig config;
    config.module_name = module_name;
    config.runtime_context.config.module_channel_config_path = io_config_path;
    auto module = ModuleFactoryRegistry::Instance().Create(module_name, config, &error);
    if (!module) {
      ReadyPipeProtocol::CloseFdIfOpen(&child_write_fd);
      return 2;
    }
    if (!ReadyPipeProtocol::SendReady(child_write_fd, &error)) {
      ReadyPipeProtocol::CloseFdIfOpen(&child_write_fd);
      return 3;
    }
    ReadyPipeProtocol::CloseFdIfOpen(&child_write_fd);
    return module->Run() ? 0 : 4;
  }, out_error);
  ReadyPipeProtocol::CloseFdIfOpen(&child_write_fd);
  if (!fork_ok) {
    ReadyPipeProtocol::CloseFdIfOpen(&parent_read_fd);
    return false;
  }

  const bool ready = supervisor->WaitForReadyOrTransitionFailed(
      module_name, parent_read_fd, module_entry.resource_schema.ready_timeout_ms);
  ReadyPipeProtocol::CloseFdIfOpen(&parent_read_fd);
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
  return true;
}

pid_t ForkLogModule(
    const mould::config::ParsedModuleLaunchEntry& module_entry,
    Supervisor* supervisor,
    int ready_event_fd,
    const std::unordered_map<std::string, ModulePipeSet>& all_pipes,
    std::string* out_error) {
  const std::string module_name = module_entry.module_name;
  const std::string cpu_set = module_entry.resource_schema.cpu_set;

  // Comma-separated pipe read ends for Logging_module (see FLAGS_log_pipe_sources).
  std::string pipe_fds_env;
  bool first = true;
  for (const auto& [name, ps] : all_pipes) {
    if (!first) pipe_fds_env += ",";
    first = false;
    pipe_fds_env += name + ":" + std::to_string(ps.read_fd);
  }

  // Set gflags directly so forked child inherits them.
  FLAGS_log_pipe_sources = pipe_fds_env;
  FLAGS_log_ready_event_fd = ready_event_fd;

  const bool fork_ok = supervisor->ForkModuleProcess(module_name, [module_name, cpu_set]() {
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
        while (std::getline(ss, token, ',')) {
          const int cpu = std::stoi(token);
          cpus.push_back(cpu);
        }
        if (!cpus.empty()) {
          int max_cpu = -1;
          for (int cpu : cpus) if (cpu > max_cpu) max_cpu = cpu;
          cpu_set_t* affinity_set = CPU_ALLOC(static_cast<std::size_t>(max_cpu) + 1U);
          if (affinity_set) {
            const std::size_t sz = CPU_ALLOC_SIZE(static_cast<std::size_t>(max_cpu) + 1U);
            CPU_ZERO_S(sz, affinity_set);
            for (int cpu : cpus) CPU_SET_S(cpu, sz, affinity_set);
            sched_setaffinity(0, sz, affinity_set);
            CPU_FREE(affinity_set);
          }
        }
      } catch (...) {
      }
    }

    std::string error;
    ModuleFactoryConfig config;
    config.module_name = module_name;
    auto module = ModuleFactoryRegistry::Instance().Create(module_name, config, &error);
    if (!module) {
      return 2;
    }
    return module->Run() ? 0 : 4;
  }, out_error);

  if (!fork_ok) {
    return 0;
  }
  return supervisor->ChildPidOf(module_name).value_or(0);
}

}  // namespace mould::comm
