#include "logging_module_gflags.hpp"

#include <gflags/gflags.h>
#include <string>

DEFINE_string(log_archive_dir, "/var/log/mould",
              "Logging_module module_params.log_archive_dir: final log archive directory");
DEFINE_string(log_tmp_dir, "/dev/shm/mould_log",
              "Logging_module module_params.log_tmp_dir: temp log directory");
DEFINE_int32(log_queue_size, 4096,
             "Logging_module module_params.log_queue_size: in-memory queue capacity");
DEFINE_int64(log_max_file_size, 10485760,
             "Logging_module module_params.log_max_file_size: max single log file bytes");
DEFINE_int32(log_max_file_count, 10,
             "Logging_module module_params.log_max_file_count: retained history files");
DEFINE_bool(log_compression_enabled, false,
            "Logging_module module_params.log_compression_enabled: gzip rotated logs");
// log_pipe_sources / log_ready_event_fd: defined in logging (logging_fork_gflags.cpp)
// so logging_core targets link without pulling backend objects.

namespace mould::backend {

void ApplyLoggingModulePipeForkGflags(const std::string& pipe_sources_csv, int ready_event_fd) {
  google::SetCommandLineOption("log_pipe_sources", pipe_sources_csv.c_str());
  google::SetCommandLineOption("log_ready_event_fd", std::to_string(ready_event_fd).c_str());
}

}  // namespace mould::backend
