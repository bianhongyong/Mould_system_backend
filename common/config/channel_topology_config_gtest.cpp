#include "channel_topology_config.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

namespace {

std::string WriteTempConfigFile(const std::string& file_name, const std::string& content) {
  const std::string path = "/tmp/" + file_name;
  std::ofstream output(path);
  output << content;
  output.close();
  return path;
}

TEST(ChannelTopologyConfigGtest, ParseModuleChannelConfigFileAcceptsJsonFormat) {
  const std::string path = WriteTempConfigFile(
      "channel_topology_parser_json_autodetect_test.json",
      R"({
  "input_channel": {
    "frame.raw": {
      "slot_payload_bytes": "8"
    }
  },
  "output_channel": {
    "feature.vec": {
      "slot_payload_bytes": "8",
      "shm_consumer_slots": "4"
    }
  }
})");

  mould::config::ModuleChannelConfig config;
  std::string error;
  const bool ok = mould::config::ParseModuleChannelConfigFile("FeatureExtractModule", path, &config, &error);
  std::remove(path.c_str());

  ASSERT_TRUE(ok) << error;
  ASSERT_EQ(config.module_name, "FeatureExtractModule");
  ASSERT_EQ(config.input_channels.size(), 1U);
  ASSERT_EQ(config.output_channels.size(), 1U);
  EXPECT_EQ(config.input_channels.front().channel, "frame.raw");
  EXPECT_EQ(config.output_channels.front().channel, "feature.vec");
  EXPECT_EQ(config.input_channels.front().params.at("slot_payload_bytes"), "8");
  EXPECT_EQ(config.output_channels.front().params.at("slot_payload_bytes"), "8");
  EXPECT_EQ(config.output_channels.front().params.at("shm_consumer_slots"), "4");
}

TEST(ChannelSchema, StrictNameEqualsProtoMessage_NoAlias) {
  mould::config::ChannelTopologyEntry ok_entry;
  ok_entry.channel = "CameraFrame";
  ok_entry.params["proto_message"] = "CameraFrame";

  std::string error;
  EXPECT_TRUE(mould::config::ValidateChannelSchemaGovernance(ok_entry, &error)) << error;

  mould::config::ChannelTopologyEntry mismatch;
  mismatch.channel = "camera.frame";
  mismatch.params["proto_message"] = "CameraFrame";
  error.clear();
  EXPECT_FALSE(mould::config::ValidateChannelSchemaGovernance(mismatch, &error));
  EXPECT_NE(error.find("must equal protobuf message name"), std::string::npos);
}

TEST(ChannelSchema, UnconfiguredChannelDefaultsToProtobuf) {
  mould::config::ChannelTopologyEntry entry;
  entry.channel = "DefaultProtoMessage";

  EXPECT_EQ(
      mould::config::ResolveChannelPayloadType(entry),
      mould::config::ChannelPayloadType::kProtobuf);
  std::string error;
  EXPECT_TRUE(mould::config::ValidateChannelSchemaGovernance(entry, &error)) << error;
}

TEST(ImageChannelSlotProfile, UsesDedicatedCapacity) {
  mould::config::ChannelTopologyEntry image_entry;
  image_entry.channel = "ImageFrame";
  image_entry.params["payload_type"] = "image";
  image_entry.params["slot_payload_bytes"] = "3145728";

  mould::config::ChannelTopologyEntry normal_entry;
  normal_entry.channel = "NormalMessage";
  normal_entry.params["slot_payload_bytes"] = "128";

  EXPECT_EQ(
      mould::config::ResolveSlotPayloadBytesForChannel(&image_entry, 64),
      3145728U);
  EXPECT_EQ(
      mould::config::ResolveSlotPayloadBytesForChannel(&normal_entry, 64),
      128U);
}

TEST(ProtobufEvolution, ReservedFieldNumber_NotReusable) {
  const std::string proto = R"(syntax = "proto3";
message Broken {
  reserved 3;
  optional string old_name = 3;
}
)";

  std::string error;
  EXPECT_FALSE(mould::config::ValidateProtoReservedFieldNumbers(proto, &error));
  EXPECT_NE(error.find("reserved"), std::string::npos);
}

TEST(ChannelTopologyConfigGtest, ShmSlotCountPerChannelOverridesDefault) {
  mould::config::ChannelTopologyEntry entry;
  entry.channel = "feature.vec";
  entry.params["shm_slot_count"] = "32";

  EXPECT_EQ(
      mould::config::ResolveShmSlotCountForChannel(&entry, 256),
      32U);
}

TEST(ShmBusDeliveryMode, BuildTopologyRejectsInvalidDeliveryMode) {
  mould::config::ModuleChannelConfig mod;
  mod.module_name = "producer_mod";
  mould::config::ChannelEndpointConfig out;
  out.channel = "tasks.queue";
  out.params["delivery_mode"] = "not_a_mode";
  mod.output_channels.push_back(out);
  mould::config::ChannelTopologyIndex topo;
  std::string err;
  EXPECT_FALSE(mould::config::BuildChannelTopologyIndex({mod}, &topo, &err));
  EXPECT_NE(err.find("invalid delivery_mode"), std::string::npos);
}

TEST(ShmBusDeliveryMode, ResolveFromChannelEntry) {
  mould::config::ChannelTopologyEntry e;
  e.channel = "x";
  e.params["delivery_mode"] = "COMPETE";
  EXPECT_EQ(
      mould::config::ResolveShmBusDeliveryModeForChannel(&e),
      mould::config::ShmBusDeliveryMode::kCompete);
}

}  // namespace
