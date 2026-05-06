#include "log_pipe_manager.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <string>
#include <unordered_map>
#include <utility>

namespace mould::comm {

bool CreateLogPipe(ModulePipeSet* out_pipe, std::string* out_error) {
  int fds[2] = {-1, -1};
  if (::pipe(fds) < 0) {
    if (out_error != nullptr) {
      *out_error = "pipe() failed errno=" + std::to_string(errno);
    }
    return false;
  }
  // Set CLOEXEC on both ends so exec'd children (if any) don't inherit them.
  for (int fd : {fds[0], fds[1]}) {
    int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0) {
      ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
  }
  out_pipe->read_fd = fds[0];
  out_pipe->write_fd = fds[1];
  return true;
}

void CloseOtherWriteFds(const std::string& keep_module,
                        const std::unordered_map<std::string, ModulePipeSet>& all_pipes) {
  for (const auto& [name, ps] : all_pipes) {
    if (name != keep_module && ps.write_fd >= 0) {
      ::close(ps.write_fd);
    }
  }
}

void CloseAllPipeFds(std::unordered_map<std::string, ModulePipeSet>* pipes) {
  if (pipes == nullptr) return;
  for (auto& [name, ps] : *pipes) {
    if (ps.read_fd >= 0) ::close(ps.read_fd);
    if (ps.write_fd >= 0) ::close(ps.write_fd);
    ps.read_fd = -1;
    ps.write_fd = -1;
  }
  pipes->clear();
}

}  // namespace mould::comm
