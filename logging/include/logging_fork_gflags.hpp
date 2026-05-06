#pragma once

#include <gflags/gflags.h>

// Owned by comm_core (logging_fork_gflags.cpp). Set by module_launcher before
// fork/exec of Logging_module; read in the child after google::ParseCommandLineFlags.
DECLARE_string(log_pipe_sources);
DECLARE_int32(log_ready_event_fd);
