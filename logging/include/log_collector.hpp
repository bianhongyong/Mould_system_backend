#pragma once

#include "log_dump_manager.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mould::comm {

struct PipeSource {
  int read_fd = -1;
  std::string module_name;
};

class LogCollector {
 public:
  using AddLogFn = std::function<void(const std::string& module_name,
                                       std::shared_ptr<const std::string> line)>;

  explicit LogCollector(AddLogFn add_log_fn);
  ~LogCollector();

  LogCollector(const LogCollector&) = delete;
  LogCollector& operator=(const LogCollector&) = delete;

  bool AddSource(PipeSource source, std::string* out_error);
  bool RemoveSource(const std::string& module_name);
  void Stop();

  std::size_t SourceCount() const;

 private:
  struct SourceState {
    int fd = -1;
    std::string module_name;
    std::string buffer;  // partial line buffer
  };

  void PollLoop();
  int FindStateIndex(SourceState* target);
  void HandleRead(int fd_idx);
  void HandleHangup(int fd_idx);

  AddLogFn add_log_fn_;
  std::atomic<bool> stopped_{false};
  std::thread poll_thread_;

  mutable std::mutex sources_mutex_;
  std::vector<std::unique_ptr<SourceState>> sources_;
  int epoll_fd_ = -1;
};

}  // namespace mould::comm
