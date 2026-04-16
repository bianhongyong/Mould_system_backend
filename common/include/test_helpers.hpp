#pragma once

#include <glog/logging.h>
#include <string>

inline bool Check(bool condition, const std::string& message) {
  if (condition) {
    return true;
  }
  LOG(ERROR) << "[FAIL] " << message;
  return false;
}
