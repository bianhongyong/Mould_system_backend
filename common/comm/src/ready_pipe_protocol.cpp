#include "ready_pipe_protocol.hpp"

#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace mould::comm {

bool ReadyPipeProtocol::CreateParentManagedPipe(
    int* out_parent_read_fd,
    int* out_child_write_fd,
    std::string* out_error) {
  if (out_parent_read_fd == nullptr || out_child_write_fd == nullptr) {
    if (out_error != nullptr) {
      *out_error = "ready pipe requires non-null fd output pointers";
    }
    return false;
  }
  int fds[2] = {-1, -1};
  if (pipe(fds) != 0) {
    if (out_error != nullptr) {
      *out_error = "ready pipe create failed: " + std::string(std::strerror(errno));
    }
    return false;
  }
  *out_parent_read_fd = fds[0];
  *out_child_write_fd = fds[1];
  return true;
}

bool ReadyPipeProtocol::SendReady(int child_write_fd, std::string* out_error) {
  if (child_write_fd < 0) {
    if (out_error != nullptr) {
      *out_error = "ready pipe write fd is invalid";
    }
    return false;
  }
  const Message message{};
  const ssize_t written = write(child_write_fd, &message, sizeof(message));
  if (written != static_cast<ssize_t>(sizeof(message))) {
    if (out_error != nullptr) {
      *out_error = "failed to write READY message to pipe";
    }
    return false;
  }
  return true;
}

bool ReadyPipeProtocol::WaitReady(int parent_read_fd, std::int64_t ready_timeout_ms, std::string* out_error) {
  if (parent_read_fd < 0) {
    if (out_error != nullptr) {
      *out_error = "ready pipe read fd is invalid";
    }
    return false;
  }
  if (ready_timeout_ms < 0) {
    if (out_error != nullptr) {
      *out_error = "ready timeout must be non-negative";
    }
    return false;
  }
  pollfd pfd{};
  pfd.fd = parent_read_fd;
  pfd.events = POLLIN;
  const int poll_result = poll(&pfd, 1, static_cast<int>(ready_timeout_ms));
  if (poll_result == 0) {
    if (out_error != nullptr) {
      *out_error = "READY timeout";
    }
    return false;
  }
  if (poll_result < 0) {
    if (out_error != nullptr) {
      *out_error = "ready poll failed: " + std::string(std::strerror(errno));
    }
    return false;
  }

  Message message{};
  const ssize_t read_size = read(parent_read_fd, &message, sizeof(message));
  if (read_size != static_cast<ssize_t>(sizeof(message))) {
    if (out_error != nullptr) {
      *out_error = "ready protocol read size mismatch";
    }
    return false;
  }
  if (message.magic != kMagic || message.version != kVersion || message.type != kTypeReady) {
    if (out_error != nullptr) {
      *out_error = "invalid READY message payload";
    }
    return false;
  }
  return true;
}

void ReadyPipeProtocol::CloseFdIfOpen(int* fd) {
  if (fd != nullptr && *fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}

}  // namespace mould::comm
