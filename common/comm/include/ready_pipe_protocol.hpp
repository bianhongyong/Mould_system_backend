#pragma once

#include <cstdint>
#include <string>

namespace mould::comm {

class ReadyPipeProtocol {
 public:
  static constexpr std::uint32_t kMagic = 0x4D4F554C;  // "MOUL"
  static constexpr std::uint16_t kVersion = 1;
  static constexpr std::uint16_t kTypeReady = 1;

  struct Message {
    std::uint32_t magic = kMagic;
    std::uint16_t version = kVersion;
    std::uint16_t type = kTypeReady;
  };

  static bool CreateParentManagedPipe(int* out_parent_read_fd, int* out_child_write_fd, std::string* out_error);
  static bool SendReady(int child_write_fd, std::string* out_error);
  static bool WaitReady(int parent_read_fd, std::int64_t ready_timeout_ms, std::string* out_error);
  static void CloseFdIfOpen(int* fd);
};

}  // namespace mould::comm
