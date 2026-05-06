#include "log_sink.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <string>

namespace mould::comm {

LogDumpManagerSink::LogDumpManagerSink(std::shared_ptr<LogDumpManager> manager,
                                       std::string module_name)
    : manager_(std::move(manager)), module_name_(std::move(module_name)) {
  google::AddLogSink(this);
}

LogDumpManagerSink::~LogDumpManagerSink() {
  google::RemoveLogSink(this);
}

void LogDumpManagerSink::send(google::LogSeverity /*severity*/,
                              const char* /*full_filename*/,
                              const char* /*base_filename*/,
                              int /*line*/,
                              const struct ::tm* /*tm_time*/,
                              const char* message,
                              std::size_t message_len) {
  if (manager_ == nullptr || message == nullptr || message_len == 0) {
    return;
  }

  // Build line content as shared string.
  auto line = std::make_shared<const std::string>(message, message_len);
  manager_->AddLog(module_name_, std::move(line));
}

void LogDumpManagerSink::WaitForLastLog() {
  // Best-effort: trigger a flush barrier to ensure pending log items
  // from the sink are processed. This is intentionally not a full barrier
  // to avoid re-entrancy concerns.
  if (manager_) {
    manager_->FlushBarrier();
  }
}

}  // namespace mould::comm
