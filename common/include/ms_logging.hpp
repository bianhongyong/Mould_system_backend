#pragma once

#include <gflags/gflags.h>
#include <glog/logging.h>

namespace mould {

// Call once per process before emitting LOG_* (including from shared libraries).
inline void InitApplicationLogging(const char* argv0) {
  google::InitGoogleLogging(argv0);
  FLAGS_logtostderr = 1;
}

inline void ShutdownApplicationLogging() { google::ShutdownGoogleLogging(); }

}  // namespace mould
