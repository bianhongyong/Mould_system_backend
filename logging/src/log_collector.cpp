#include "log_collector.hpp"

#include <sys/epoll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <glog/logging.h>
#include <utility>

namespace mould::comm {

namespace {

constexpr int kMaxEvents = 64;
constexpr std::size_t kReadBufSize = 65536;

}  // namespace

LogCollector::LogCollector(AddLogFn add_log_fn)
    : add_log_fn_(std::move(add_log_fn)) {
  epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ < 0) {
    LOG(FATAL) << "[LogCollector] epoll_create1 failed errno=" << errno;
  }
  poll_thread_ = std::thread(&LogCollector::PollLoop, this);
}

LogCollector::~LogCollector() {
  Stop();
  if (epoll_fd_ >= 0) {
    ::close(epoll_fd_);
  }
}

bool LogCollector::AddSource(PipeSource source, std::string* out_error) {
  if (source.read_fd < 0) {
    if (out_error != nullptr) {
      *out_error = "invalid read fd";
    }
    return false;
  }
  if (source.module_name.empty()) {
    if (out_error != nullptr) {
      *out_error = "module name required for pipe source";
    }
    return false;
  }

  auto state = std::make_unique<SourceState>();
  state->fd = source.read_fd;
  state->module_name = std::move(source.module_name);

  struct epoll_event ev {};
  ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
  ev.data.ptr = state.get();

  if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, state->fd, &ev) < 0) {
    if (out_error != nullptr) {
      *out_error = "epoll_ctl ADD failed errno=" + std::to_string(errno);
    }
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    sources_.push_back(std::move(state));
  }
  return true;
}

bool LogCollector::RemoveSource(const std::string& module_name) {
  std::lock_guard<std::mutex> lock(sources_mutex_);
  for (auto it = sources_.begin(); it != sources_.end(); ++it) {
    if ((*it)->module_name == module_name) {
      if ((*it)->fd >= 0) {
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, (*it)->fd, nullptr);
        ::close((*it)->fd);
      }
      sources_.erase(it);
      return true;
    }
  }
  return false;
}

void LogCollector::Stop() {
  stopped_.store(true, std::memory_order_release);
  if (poll_thread_.joinable()) {
    poll_thread_.join();
  }
}

std::size_t LogCollector::SourceCount() const {
  std::lock_guard<std::mutex> lock(sources_mutex_);
  return sources_.size();
}

int LogCollector::FindStateIndex(SourceState* target) {
  if (target == nullptr) {
    return -1;
  }
  std::lock_guard<std::mutex> lock(sources_mutex_);
  for (std::size_t i = 0; i < sources_.size(); ++i) {
    if (sources_[i].get() == target) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void LogCollector::PollLoop() {
  auto events = std::make_unique<struct epoll_event[]>(kMaxEvents);

  while (!stopped_.load(std::memory_order_acquire)) {
    int nfds = epoll_wait(epoll_fd_, events.get(), kMaxEvents, 200);
    if (nfds < 0) {
      if (errno == EINTR) {
        continue;
      }
      LOG(WARNING) << "[LogCollector] epoll_wait error errno=" << errno;
      break;
    }

    for (int i = 0; i < nfds; ++i) {
      auto* state = static_cast<SourceState*>(events[i].data.ptr);
      const int idx = FindStateIndex(state);
      if (idx < 0) {
        continue;
      }
      if (events[i].events & (EPOLLERR | EPOLLRDHUP | EPOLLHUP)) {
        HandleHangup(idx);
        continue;
      }
      if (events[i].events & EPOLLIN) {
        HandleRead(idx);
      }
    }
  }
}

void LogCollector::HandleRead(int idx) {
  SourceState* state;
  {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    if (idx < 0 || static_cast<std::size_t>(idx) >= sources_.size()) {
      return;
    }
    state = sources_[static_cast<std::size_t>(idx)].get();
  }

  char buf[kReadBufSize];
  while (!stopped_.load(std::memory_order_acquire)) {
    ssize_t n = ::read(state->fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      if (errno == EINTR) {
        continue;
      }
      // Read error on this source - treat as hangup.
      HandleHangup(idx);
      return;
    }
    if (n == 0) {
      HandleHangup(idx);
      return;
    }

    // Append to line buffer.
    state->buffer.append(buf, static_cast<std::size_t>(n));

    // Process complete lines.
    std::size_t pos = 0;
    while (true) {
      auto newline_pos = state->buffer.find('\n', pos);
      if (newline_pos == std::string::npos) {
        break;
      }
      std::string line(state->buffer, pos, newline_pos - pos);
      pos = newline_pos + 1;

      if (add_log_fn_) {
        add_log_fn_(state->module_name,
                    std::make_shared<const std::string>(std::move(line)));
      }
    }

    // Keep remaining partial line.
    if (pos > 0) {
      state->buffer.erase(0, pos);
    }
  }
}

void LogCollector::HandleHangup(int idx) {
  SourceState* state;
  {
    std::lock_guard<std::mutex> lock(sources_mutex_);
    if (idx < 0 || static_cast<std::size_t>(idx) >= sources_.size()) {
      return;
    }
    state = sources_[static_cast<std::size_t>(idx)].get();
  }

  LOG(INFO) << "[LogCollector] pipe hangup module=" << state->module_name;

  // Flush remaining partial line.
  if (!state->buffer.empty()) {
    if (add_log_fn_) {
      add_log_fn_(state->module_name,
                  std::make_shared<const std::string>(std::move(state->buffer)));
    }
    state->buffer.clear();
  }

  // Remove from epoll and close fd.
  if (state->fd >= 0) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, state->fd, nullptr);
    ::close(state->fd);
    state->fd = -1;
  }
}

}  // namespace mould::comm
