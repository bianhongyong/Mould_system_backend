#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace mould::comm {

struct ShmSegmentLayout {
  std::uint32_t magic = 0x4d4f554c;  // "MOUL"
  std::uint16_t version = 1;
  std::size_t payload_capacity = 0;
};

class ShmSegment {
 public:
  ShmSegment() = default;
  ShmSegment(const ShmSegment&) = delete;
  ShmSegment& operator=(const ShmSegment&) = delete;
  ShmSegment(ShmSegment&& other) noexcept;
  ShmSegment& operator=(ShmSegment&& other) noexcept;
  ~ShmSegment();

  static std::optional<ShmSegment> CreateOrAttach(
      const std::string& channel,
      const ShmSegmentLayout& layout);

  const std::string& Name() const;
  std::size_t SizeBytes() const;
  void* BaseAddress() const;
  bool IsOwner() const;

 private:
  ShmSegment(std::string name, int fd, void* base_address, std::size_t size_bytes, bool owner);
  void Reset();

  std::string name_;
  int fd_ = -1;
  void* base_address_ = nullptr;
  std::size_t size_bytes_ = 0;
  bool owner_ = false;
};

std::string BuildDeterministicShmName(const std::string& channel);
std::size_t ShmSegmentHeaderSizeBytes();

}  // namespace mould::comm
