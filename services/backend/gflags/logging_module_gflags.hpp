#pragma once

#include <gflags/gflags.h>
#include <string>

DECLARE_string(log_archive_dir);
DECLARE_string(log_tmp_dir);
DECLARE_int32(log_queue_size);
DECLARE_int64(log_max_file_size);
DECLARE_int32(log_max_file_count);
DECLARE_bool(log_compression_enabled);
DECLARE_string(log_pipe_sources);
DECLARE_int32(log_ready_event_fd);

namespace mould::backend {

void ApplyLoggingModulePipeForkGflags(const std::string& pipe_sources_csv, int ready_event_fd);

}  // namespace mould::backend
