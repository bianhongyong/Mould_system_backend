#include "subprocess_log_init.hpp"

#include <fcntl.h>
#include <unistd.h>

namespace mould {

void RedirectStderrToLogPipe(int pipe_write_fd) {
  if (pipe_write_fd < 0) {
    return;
  }
  // dup2 pipe_write_fd to STDERR_FILENO.
  // After dup2, we close the original fd (the child's copy) since stderr
  // now refers to the pipe.
  ::dup2(pipe_write_fd, STDERR_FILENO);
  // Close the original write fd if it differs from stderr.
  if (pipe_write_fd != STDERR_FILENO) {
    ::close(pipe_write_fd);
  }
}

int SetNonBlocking(int fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

}  // namespace mould
