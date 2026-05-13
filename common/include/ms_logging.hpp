#pragma once

#include <gflags/gflags.h>
#include <glog/logging.h>
#include "muduo/base/Logging.h"

namespace mould {

// Call once per process before emitting LOG_* (including from shared libraries).
inline void InitApplicationLogging(const char* argv0) {
  google::InitGoogleLogging(argv0);
  FLAGS_logtostderr = 1;

  // muduo 库（TcpServer 等）默认输出到 stdout，将其重定向到 stderr
  // 使其被日志管道（RedirectStderrToLogPipe）统一捕获落盘
  muduo::Logger::setOutput([](const char* msg, int len) {
    fwrite(msg, 1, len, stderr);
  });
  muduo::Logger::setFlush([]() {
    fflush(stderr);
  });
}

inline void ShutdownApplicationLogging() { google::ShutdownGoogleLogging(); }

}  // namespace mould
