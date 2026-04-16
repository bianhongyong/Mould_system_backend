#pragma once

#include "interfaces.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace mould::comm {

enum class PayloadType {
  kProtobuf,
  kImage,
  kBinaryBlob,
};

struct PayloadDescriptor {
  PayloadType payload_type = PayloadType::kBinaryBlob;
  std::size_t size = 0;
  std::uint32_t checksum = 0;
  std::uint32_t schema_version = 1;
  std::chrono::milliseconds ttl{0};
  std::uint32_t ref_count = 0;
  std::uint32_t storage_flags = 0;
};

struct PayloadPacket {
  struct ImageMetadata {
    std::string image_name;
    bool lamination_detect_flag = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t channels = 0;
    std::string format;
    std::uint64_t timestamp = 0;
  };

  struct ShmSlotHeader {
    std::uint64_t sequence = 0;
    std::uint32_t state = 0;
    PayloadType payload_type = PayloadType::kBinaryBlob;
    std::size_t payload_size = 0;
    std::uint32_t checksum = 0;
    std::uint32_t schema_version = 1;
    std::uint64_t ttl_deadline = 0;
    std::uint32_t ref_count = 0;
    std::uint32_t storage_flags = 0;
    std::optional<ImageMetadata> image_metadata;
  };

  PayloadDescriptor descriptor;
  ShmSlotHeader shm_slot;
  ByteBuffer data;
};

class IPayloadCodec {
 public:
  virtual ~IPayloadCodec() = default;
  virtual PayloadType Type() const = 0;
  virtual bool Encode(const ByteBuffer& input, PayloadPacket* packet) const = 0;
  virtual bool Decode(const PayloadPacket& packet, ByteBuffer* output) const = 0;
};

class ProtobufPayloadCodec final : public IPayloadCodec {
 public:
  PayloadType Type() const override;
  bool Encode(const ByteBuffer& input, PayloadPacket* packet) const override;
  bool Decode(const PayloadPacket& packet, ByteBuffer* output) const override;
};

class ImagePayloadCodec final : public IPayloadCodec {
 public:
  PayloadType Type() const override;
  bool Encode(const ByteBuffer& input, PayloadPacket* packet) const override;
  bool Decode(const PayloadPacket& packet, ByteBuffer* output) const override;

  bool EncodeWithMetadata(
      const ByteBuffer& input,
      const PayloadPacket::ImageMetadata& metadata,
      PayloadPacket* packet) const;
};

class BinaryBlobCodec final : public IPayloadCodec {
 public:
  PayloadType Type() const override;
  bool Encode(const ByteBuffer& input, PayloadPacket* packet) const override;
  bool Decode(const PayloadPacket& packet, ByteBuffer* output) const override;
};

class PayloadCodecFactory {
 public:
  void Register(std::shared_ptr<IPayloadCodec> codec);
  std::shared_ptr<IPayloadCodec> Get(PayloadType type) const;

  static PayloadCodecFactory CreateDefault();

 private:
  std::map<PayloadType, std::shared_ptr<IPayloadCodec>> codecs_;
};

}  // namespace mould::comm
