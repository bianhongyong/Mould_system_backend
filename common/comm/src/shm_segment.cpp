#include "shm_segment.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mould::comm {

namespace {

struct SegmentHeader {
  std::uint32_t magic = 0;
  std::uint16_t version = 0;
  std::uint16_t reserved = 0;
  std::uint64_t payload_capacity = 0;
};

std::size_t ComputeSegmentSize(const ShmSegmentLayout& layout) {
  return sizeof(SegmentHeader) + layout.payload_capacity;
}

bool IsNameCharAllowed(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
      (value >= '0' && value <= '9') || value == '_' || value == '-';
}

}  // namespace

std::string BuildDeterministicShmName(const std::string& channel) {
  std::string sanitized;
  sanitized.reserve(channel.size());
  for (const char value : channel) {
    sanitized.push_back(IsNameCharAllowed(value) ? value : '_');
  }
  if (sanitized.empty()) {
    sanitized = "default";
  }
  return "/mould_comm_" + sanitized;
}

std::size_t ShmSegmentHeaderSizeBytes() {
  return sizeof(SegmentHeader);
}

ShmSegment::ShmSegment(std::string name, int fd, void* base_address, std::size_t size_bytes, bool owner)
    : name_(std::move(name)),
      fd_(fd),
      base_address_(base_address),
      size_bytes_(size_bytes),
      owner_(owner) {}

ShmSegment::ShmSegment(ShmSegment&& other) noexcept {
  *this = std::move(other);
}

ShmSegment& ShmSegment::operator=(ShmSegment&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  Reset();
  name_ = std::move(other.name_);
  fd_ = other.fd_;
  base_address_ = other.base_address_;
  size_bytes_ = other.size_bytes_;
  owner_ = other.owner_;
  other.fd_ = -1;
  other.base_address_ = nullptr;
  other.size_bytes_ = 0;
  other.owner_ = false;
  return *this;
}

ShmSegment::~ShmSegment() {
  Reset();
}

std::optional<ShmSegment> ShmSegment::CreateOrAttach(const std::string& channel, const ShmSegmentLayout& layout) {
  const std::string shm_name = BuildDeterministicShmName(channel);
  const std::size_t total_size = ComputeSegmentSize(layout);
  if (total_size < sizeof(SegmentHeader)) {
    return std::nullopt;
  }
  
  bool owner = false;
  int fd = shm_open(shm_name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd >= 0) {
    owner = true;
    if (ftruncate(fd, static_cast<off_t>(total_size)) != 0) {
      close(fd);
      shm_unlink(shm_name.c_str());
      return std::nullopt;
    }
  } else if (errno == EEXIST) {
    fd = shm_open(shm_name.c_str(), O_RDWR, 0600);
    if (fd < 0) {
      return std::nullopt;
    }
  } else {
    return std::nullopt;
  }

  void* base_address = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (base_address == MAP_FAILED) {
    close(fd);
    if (owner) {
      shm_unlink(shm_name.c_str());
    }
    return std::nullopt;
  }

  auto* header = static_cast<SegmentHeader*>(base_address);
  if (owner) {
    std::memset(base_address, 0, total_size);
    header->magic = layout.magic;
    header->version = layout.version;
    header->payload_capacity = layout.payload_capacity;
  } else {
    if (header->magic != layout.magic || header->version != layout.version) {
      munmap(base_address, total_size);
      close(fd);
      return std::nullopt;
    }
  }

  return ShmSegment(shm_name, fd, base_address, total_size, owner);
}

std::optional<ShmSegment> ShmSegment::Attach(const std::string& channel, const ShmSegmentLayout& layout) {
  const std::string shm_name = BuildDeterministicShmName(channel);
  const std::size_t expected_total = ComputeSegmentSize(layout);
  if (expected_total < sizeof(SegmentHeader)) {
    return std::nullopt;
  }

  const int fd = shm_open(shm_name.c_str(), O_RDWR, 0);
  if (fd < 0) {
    return std::nullopt;
  }

  struct stat st {};
  if (fstat(fd, &st) != 0) {
    close(fd);
    return std::nullopt;
  }
  const auto total_size = static_cast<std::size_t>(st.st_size);
  if (total_size != expected_total) {
    close(fd);
    return std::nullopt;
  }

  void* base_address = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (base_address == MAP_FAILED) {
    close(fd);
    return std::nullopt;
  }

  const auto* header = static_cast<const SegmentHeader*>(base_address);
  if (header->magic != layout.magic || header->version != layout.version ||
      header->payload_capacity != layout.payload_capacity) {
    munmap(base_address, total_size);
    close(fd);
    return std::nullopt;
  }

  return ShmSegment(shm_name, fd, base_address, total_size, false);
}

const std::string& ShmSegment::Name() const {
  return name_;
}

std::size_t ShmSegment::SizeBytes() const {
  return size_bytes_;
}

void* ShmSegment::BaseAddress() const {
  return base_address_;
}

bool ShmSegment::IsOwner() const {
  return owner_;
}

void ShmSegment::Reset() {
  if (base_address_ != nullptr && size_bytes_ > 0) {
    munmap(base_address_, size_bytes_);
  }
  if (fd_ >= 0) {
    close(fd_);
  }
  base_address_ = nullptr;
  fd_ = -1;
  size_bytes_ = 0;
  owner_ = false;
}

}  // namespace mould::comm
