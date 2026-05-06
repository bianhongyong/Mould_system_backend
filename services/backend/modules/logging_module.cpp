#include "log_collector.hpp"
#include "log_dump_manager.hpp"
#include "log_sink.hpp"
#include "module_factory_registry.hpp"
#include "gflags/logging_module_gflags.hpp"
#include "subprocess_log_init.hpp"

#include <glog/logging.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

namespace mould::backend {

using mould::comm::LogCollector;
using mould::comm::LogDumpManager;
using mould::comm::LogDumpManagerConfig;
using mould::comm::LogDumpManagerSink;
using mould::comm::ModuleBase;
using mould::comm::ModuleFactoryConfig;

namespace {

// Eventfd used to signal readiness to the main process.
// Main process expects the log module to send a byte on this fd before
// proceeding to launch other modules.
int g_ready_event_fd = -1;

// Shared manager and collector, owned by the module.
std::shared_ptr<LogDumpManager> g_manager;
std::unique_ptr<LogCollector> g_collector;
std::unique_ptr<LogDumpManagerSink> g_sink;

// Config from gflags (parent sets FLAGS before fork; child inherits).
LogDumpManagerConfig LoadConfigFromFlags() {
  LogDumpManagerConfig cfg;
  cfg.log_dir = FLAGS_log_archive_dir;
  cfg.log_tmp_dir = FLAGS_log_tmp_dir;
  if (FLAGS_log_queue_size > 0) {
    cfg.queue_size = static_cast<std::size_t>(FLAGS_log_queue_size);
  }
  if (FLAGS_log_max_file_size > 0) {
    cfg.max_file_size = static_cast<std::size_t>(FLAGS_log_max_file_size);
  }
  if (FLAGS_log_max_file_count > 0) {
    cfg.max_file_count = static_cast<std::size_t>(FLAGS_log_max_file_count);
  }
  cfg.compression_enabled = FLAGS_log_compression_enabled;
  g_ready_event_fd = FLAGS_log_ready_event_fd;

  return cfg;
}

}  // namespace

bool SetupPipeSourcesFromFlags(LogCollector* collector) {
  const std::string& sources_csv = FLAGS_log_pipe_sources;
  if (sources_csv.empty()) {
    LOG(WARNING) << "[Logging_module] log_pipe_sources empty, no pipe sources";
    return true;
  }

  std::istringstream ss(sources_csv);
  std::string token;
  while (std::getline(ss, token, ',')) {
    auto colon_pos = token.find(':');
    if (colon_pos == std::string::npos || colon_pos == 0 || colon_pos == token.size() - 1) {
      LOG(WARNING) << "[Logging_module] invalid pipe source entry: " << token;
      continue;
    }
    std::string module_name = token.substr(0, colon_pos);
    int fd = std::atoi(token.substr(colon_pos + 1).c_str());
    if (fd < 0) {
      LOG(WARNING) << "[Logging_module] invalid fd for module " << module_name;
      continue;
    }
    // Remove CLOEXEC flag since this fd is meant to be inherited by the log module.
    int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0 && (flags & FD_CLOEXEC)) {
      ::fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC);
    }
    // Set non-blocking for reads.
    mould::SetNonBlocking(fd);

    mould::comm::PipeSource source;
    source.read_fd = fd;
    source.module_name = std::move(module_name);
    std::string error;
    if (!collector->AddSource(std::move(source), &error)) {
      LOG(WARNING) << "[Logging_module] failed to add pipe source: " << error;
    }
  }
  return true;
}

class Logging_module final : public ModuleBase {
 public:
  explicit Logging_module(const ModuleFactoryConfig& config)
      : ModuleBase(config.module_name, config.runtime_context) {}
  ~Logging_module() override {
    Stop();
  }

 private:
  bool DoInit() override {
    LogDumpManagerConfig cfg = LoadConfigFromFlags();

    g_manager = std::make_shared<LogDumpManager>(std::move(cfg));
    std::string init_error;
    if (!g_manager->Init(&init_error)) {
      LOG(ERROR) << "[Logging_module] LogDumpManager init failed: " << init_error;
      return false;
    }

    g_collector = std::make_unique<LogCollector>(
        [](const std::string& module_name,
           std::shared_ptr<const std::string> line) {
          if (g_manager) {
            g_manager->AddLog(module_name, std::move(line));
          }
        });

    // Register LogSink for local glog capture.
    // Use the module's own name as the module tag for local logs.
    g_sink = std::make_unique<LogDumpManagerSink>(g_manager, "Logging_module");

    SetupPipeSourcesFromFlags(g_collector.get());

    LOG(INFO) << "[Logging_module] LogDumpManager initialized, collector started";

    // Signal readiness to main process.
    if (g_ready_event_fd >= 0) {
      const std::uint64_t ready_val = 1;
      ssize_t written = ::write(g_ready_event_fd, &ready_val, sizeof(ready_val));
      if (written < 0) {
        LOG(WARNING) << "[Logging_module] failed to write ready event errno=" << errno;
      }
      ::close(g_ready_event_fd);
      g_ready_event_fd = -1;
    }

    return true;
  }

  bool SetupSubscriptions() override {
    // Logging_module has no SHM subscriptions.
    return true;
  }

  void OnRunIteration() override {
    // Collector runs on its own thread via epoll; nothing to do in the main loop.
    static constexpr auto kCheckInterval = std::chrono::milliseconds(1000);
    std::this_thread::sleep_for(kCheckInterval);
  }

  void Stop() {
    if (g_sink) {
      g_sink.reset();
    }
    if (g_collector) {
      g_collector->Stop();
      g_collector.reset();
    }
    if (g_manager) {
      g_manager->Stop();
      g_manager.reset();
    }
  }
};

REGISTER_MOULD_MODULE_AS("Logging_module", Logging_module)

}  // namespace mould::backend
