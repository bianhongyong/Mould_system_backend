#include "payload_codec.hpp"

namespace mould::comm {

namespace {

std::uint32_t SimpleChecksum(const ByteBuffer& bytes) {
  std::uint32_t checksum = 0;
  for (std::uint8_t byte : bytes) {
    checksum = (checksum * 131U) + byte;
  }
  return checksum;
}

std::uint64_t ComputeTtlDeadlineMs(std::chrono::milliseconds ttl) {
  if (ttl.count() <= 0) {
    return 0;
  }
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  return static_cast<std::uint64_t>(now_ms + ttl.count());
}

bool EncodeByType(PayloadType type, const ByteBuffer& input, PayloadPacket* packet) {
  if (!packet) {
    return false;
  }
  if (packet->descriptor.ttl.count() <= 0) {
    packet->descriptor.ttl = std::chrono::milliseconds(1000);
  }
  packet->descriptor.payload_type = type;
  packet->descriptor.size = input.size();
  packet->descriptor.checksum = SimpleChecksum(input);
  packet->descriptor.ref_count = 1;
  packet->shm_slot.payload_type = packet->descriptor.payload_type;
  packet->shm_slot.payload_size = packet->descriptor.size;
  packet->shm_slot.checksum = packet->descriptor.checksum;
  packet->shm_slot.schema_version = packet->descriptor.schema_version;
  packet->shm_slot.ttl_deadline = ComputeTtlDeadlineMs(packet->descriptor.ttl);
  packet->shm_slot.ref_count = packet->descriptor.ref_count;
  packet->shm_slot.storage_flags = packet->descriptor.storage_flags;
  packet->data = input;
  return true;
}

bool DecodeByType(PayloadType type, const PayloadPacket& packet, ByteBuffer* output) {
  if (!output || packet.descriptor.payload_type != type) {
    return false;
  }
  if (packet.descriptor.size != packet.data.size()) {
    return false;
  }
  if (packet.descriptor.checksum != SimpleChecksum(packet.data)) {
    return false;
  }
  if (packet.shm_slot.payload_type != packet.descriptor.payload_type ||
      packet.shm_slot.payload_size != packet.descriptor.size ||
      packet.shm_slot.checksum != packet.descriptor.checksum ||
      packet.shm_slot.schema_version != packet.descriptor.schema_version ||
      packet.shm_slot.ref_count != packet.descriptor.ref_count ||
      packet.shm_slot.storage_flags != packet.descriptor.storage_flags) {
    return false;
  }
  *output = packet.data;
  return true;
}

}  // namespace

PayloadType ProtobufPayloadCodec::Type() const {
  return PayloadType::kProtobuf;
}

bool ProtobufPayloadCodec::Encode(const ByteBuffer& input, PayloadPacket* packet) const {
  return EncodeByType(Type(), input, packet);
}

bool ProtobufPayloadCodec::Decode(const PayloadPacket& packet, ByteBuffer* output) const {
  return DecodeByType(Type(), packet, output);
}

PayloadType ImagePayloadCodec::Type() const {
  return PayloadType::kImage;
}

bool ImagePayloadCodec::Encode(const ByteBuffer& input, PayloadPacket* packet) const {
  return EncodeWithMetadata(input, PayloadPacket::ImageMetadata{}, packet);
}

bool ImagePayloadCodec::Decode(const PayloadPacket& packet, ByteBuffer* output) const {
  return DecodeByType(Type(), packet, output);
}

bool ImagePayloadCodec::EncodeWithMetadata(
    const ByteBuffer& input,
    const PayloadPacket::ImageMetadata& metadata,
    PayloadPacket* packet) const {
  if (!EncodeByType(Type(), input, packet)) {
    return false;
  }
  packet->shm_slot.image_metadata = metadata;
  return true;
}

PayloadType BinaryBlobCodec::Type() const {
  return PayloadType::kBinaryBlob;
}

bool BinaryBlobCodec::Encode(const ByteBuffer& input, PayloadPacket* packet) const {
  return EncodeByType(Type(), input, packet);
}

bool BinaryBlobCodec::Decode(const PayloadPacket& packet, ByteBuffer* output) const {
  return DecodeByType(Type(), packet, output);
}

void PayloadCodecFactory::Register(std::shared_ptr<IPayloadCodec> codec) {
  if (!codec) {
    return;
  }
  codecs_[codec->Type()] = std::move(codec);
}

std::shared_ptr<IPayloadCodec> PayloadCodecFactory::Get(PayloadType type) const {
  const auto iter = codecs_.find(type);
  if (iter == codecs_.end()) {
    return nullptr;
  }
  return iter->second;
}

PayloadCodecFactory PayloadCodecFactory::CreateDefault() {
  PayloadCodecFactory factory;
  factory.Register(std::make_shared<ProtobufPayloadCodec>());
  factory.Register(std::make_shared<ImagePayloadCodec>());
  factory.Register(std::make_shared<BinaryBlobCodec>());
  return factory;
}

}  // namespace mould::comm
