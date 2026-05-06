#include "logging_fork_gflags.hpp"

DEFINE_string(log_pipe_sources, "",
              "Runtime: module:read_fd,... stderr pipe read ends (set by supervisor before fork)");
DEFINE_int32(log_ready_event_fd, -1,
             "Runtime: eventfd for Logging_module ready handshake (set by supervisor before fork)");
