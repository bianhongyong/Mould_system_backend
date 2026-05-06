#pragma once

#include "log_dump_manager.hpp"

#include <glog/logging.h>

#include <memory>
#include <string>

namespace mould::comm {

class LogDumpManagerSink final : public google::LogSink {
 public:
  explicit LogDumpManagerSink(std::shared_ptr<LogDumpManager> manager,
                               std::string module_name);
  ~LogDumpManagerSink() override;

  void send(google::LogSeverity severity, const char* full_filename,
            const char* base_filename, int line,
            const struct ::tm* tm_time, const char* message,
            std::size_t message_len) override;

  void WaitForLastLog();

 private:
  std::shared_ptr<LogDumpManager> manager_;
  std::string module_name_;
};

}  // namespace mould::comm
