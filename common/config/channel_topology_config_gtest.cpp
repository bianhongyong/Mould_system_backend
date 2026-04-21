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
      "queue_depth": "8"
    }
  },
  "output_channel": {
    "feature.vec": {
      "queue_depth_per_consumer": "8"
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
  EXPECT_EQ(config.input_channels.front().params.at("queue_depth"), "8");
  EXPECT_EQ(config.output_channels.front().params.at("queue_depth_per_consumer"), "8");
}

}  // namespace
