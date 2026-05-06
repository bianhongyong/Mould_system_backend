#pragma once

#include <string>

namespace mould {

// Call in a child process (after fork) to redirect stderr to a pipe write fd
// used for unified logging. Must be called before any LOG() statement.
// The pipe write fd is dup2'd to STDERR_FILENO, then the original fd is closed.
void RedirectStderrToLogPipe(int pipe_write_fd);

// Convenience: open a pipe write fd in non-blocking mode.
// Returns 0 on success, -1 on error (errno set).
int SetNonBlocking(int fd);

}  // namespace mould
