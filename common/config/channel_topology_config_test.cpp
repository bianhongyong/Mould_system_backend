#include "ms_logging.hpp"
#include "channel_topology_config.hpp"
#include "shm_bus_control_plane.hpp"
#include "shm_bus_runtime.hpp"
#include "shm_segment.hpp"
#include "test_helpers.hpp"

#include <sys/mman.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace {

using mould::comm::BuildDeterministicShmName;
using mould::comm::MiddlewareConfig;
using mould::comm::ShmBusControlPlane;
using mould::comm::ShmBusRuntime;
using mould::config::BuildChannelTopologyIndex;
using mould::config::BuildChannelTopologyIndexFromFiles;
using mould::config::CanonicalShmChannelKey;
using mould::config::ChannelTopologyIndex;
using mould::config::ModuleChannelConfig;
using mould::config::ParseModuleChannelConfigFile;
using mould::config::ResolveShmRingConsumerCapacity;

std::string WriteTempConfig(const std::string& file_name, const std::string& content) {
  const std::string path = "/tmp/" + file_name;
  std::ofstream output(path);
  output << content;
  output.close();
  return path;
}

bool TestParseModuleTxtConfig() {
  const std::string path = WriteTempConfig(
      "module2_parser_ok.txt",
      "# parser accepts comment and params\n"
      "output broker.frames queue_depth_per_consumer=16\n"
      "input infer.results queue_depth=64\n");

  ModuleChannelConfig config;
  std::string error;
  const bool parsed = ParseModuleChannelConfigFile("broker", path, &config, &error);
  std::remove(path.c_str());

  if (!Check(parsed, "module .txt config should parse successfully")) {
    return false;
  }
  if (!Check(config.output_channels.size() == 1, "output channel count should be 1")) {
    return false;
  }
  if (!Check(config.input_channels.size() == 1, "input channel count should be 1")) {
    return false;
  }
  return Check(
      config.output_channels.front().params.at("queue_depth_per_consumer") == "16",
      "output params should keep queue_depth_per_consumer");
}

bool TestParseRejectsInvalidSyntax() {
  const std::string path = WriteTempConfig(
      "module2_parser_invalid.txt",
      "publish broker.frames queue_depth=16\n");
  ModuleChannelConfig config;
  std::string error;
  const bool parsed = ParseModuleChannelConfigFile("broker", path, &config, &error);
  std::remove(path.c_str());
  return Check(!parsed, "parser should reject unknown role keywords");
}

bool TestParseRejectsDuplicateOutputChannel() {
  const std::string path = WriteTempConfig(
      "module2_parser_duplicate_output.txt",
      "output broker.frames\n"
      "output broker.frames\n");
  ModuleChannelConfig config;
  std::string error;
  const bool parsed = ParseModuleChannelConfigFile("broker", path, &config, &error);
  std::remove(path.c_str());
  return Check(!parsed, "parser should reject duplicate output channel in one module");
}

bool TestBuildTopologyAndConflictChecks() {
  const std::string broker_path = WriteTempConfig(
      "module2_broker.txt",
      "output broker.frames queue_depth_per_consumer=8\n"
      "input infer.results\n");
  const std::string infer_path = WriteTempConfig(
      "module2_infer.txt",
      "input broker.frames queue_depth_per_consumer=8\n"
      "output infer.results\n");
  const std::string rogue_path = WriteTempConfig(
      "module2_rogue_conflict.txt",
      "output broker.frames queue_depth_per_consumer=8\n");

  ModuleChannelConfig broker;
  ModuleChannelConfig infer;
  ModuleChannelConfig rogue;
  std::string error;
  bool ok = ParseModuleChannelConfigFile("broker", broker_path, &broker, &error);
  ok = ParseModuleChannelConfigFile("infer", infer_path, &infer, &error) && ok;
  ok = ParseModuleChannelConfigFile("rogue", rogue_path, &rogue, &error) && ok;
  std::remove(broker_path.c_str());
  std::remove(infer_path.c_str());
  std::remove(rogue_path.c_str());
  if (!Check(ok, "all config files should parse in topology test")) {
    return false;
  }

  ChannelTopologyIndex topology;
  error.clear();
  if (!Check(
          BuildChannelTopologyIndex({broker, infer}, &topology, &error),
          "topology should merge module configs")) {
    return false;
  }

  const auto iter = topology.find("broker.frames");
  if (!Check(iter != topology.end(), "merged topology should contain broker.frames")) {
    return false;
  }
  if (!Check(iter->second.consumer_count == 1, "consumer_count should equal topology aggregation")) {
    return false;
  }
  if (!Check(iter->second.producers.size() == 1, "single producer channel should remain valid")) {
    return false;
  }

  error.clear();
  ChannelTopologyIndex invalid_topology;
  const bool conflict_ok = BuildChannelTopologyIndex({broker, infer, rogue}, &invalid_topology, &error);
  return Check(!conflict_ok, "multiple producers on same channel should be rejected");
}

bool TestTopologyRejectsParameterConflict() {
  const std::string module_a_path = WriteTempConfig(
      "module2_param_a.txt",
      "output broker.frames queue_depth=64\n");
  const std::string module_b_path = WriteTempConfig(
      "module2_param_b.txt",
      "input broker.frames queue_depth=128\n");

  ModuleChannelConfig module_a;
  ModuleChannelConfig module_b;
  std::string error;
  bool ok = ParseModuleChannelConfigFile("broker", module_a_path, &module_a, &error);
  ok = ParseModuleChannelConfigFile("infer", module_b_path, &module_b, &error) && ok;
  std::remove(module_a_path.c_str());
  std::remove(module_b_path.c_str());
  if (!Check(ok, "config parser should accept independent module files for conflict test")) {
    return false;
  }

  ChannelTopologyIndex topology;
  error.clear();
  const bool merged = BuildChannelTopologyIndex({module_a, module_b}, &topology, &error);
  return Check(!merged, "topology should reject conflicting parameter values");
}

bool TestBuildTopologyFromConfigFiles() {
  const std::string broker_path = WriteTempConfig(
      "module2_file_broker.json",
      R"({
  "input_channel": {},
  "output_channel": {
    "broker.frames": {"queue_depth_per_consumer": "8"}
  }
})");
  const std::string infer_path = WriteTempConfig(
      "module2_file_infer.json",
      R"({
  "input_channel": {
    "broker.frames": {"queue_depth_per_consumer": "8"}
  },
  "output_channel": {}
})");
  std::string error;
  ChannelTopologyIndex topology;
  const bool ok = BuildChannelTopologyIndexFromFiles(
      {{"broker", broker_path}, {"infer", infer_path}}, &topology, &error);
  std::remove(broker_path.c_str());
  std::remove(infer_path.c_str());
  return Check(ok, "topology should build from module config file list");
}

bool TestShmPreallocationUsesTopology() {
  const std::string channel = "module2_topology_prealloc_channel";
  ChannelTopologyIndex topology;
  topology[channel].channel = channel;
  topology[channel].producers = {"broker"};
  topology[channel].consumers = {"infer_a", "infer_b", "infer_c"};
  topology[channel].consumer_count = topology[channel].consumers.size();
  const std::string shm_name = BuildDeterministicShmName(CanonicalShmChannelKey(topology[channel]));
  shm_unlink(shm_name.c_str());
  // Per-slot payload budget is ceil(payload_region_bytes / slot_count); with the
  // current ring layout, payload_region_bytes = queue_depth * slot_count, so the
  // configured queue_depth is also the maximum per-message payload size.
  topology[channel].params["queue_depth"] = "31";

  MiddlewareConfig middleware_config;
  ShmBusControlPlane control_plane;
  ShmBusRuntime bus;
  if (!Check(control_plane.ProvisionChannelTopology(topology, middleware_config), "preallocation from topology should succeed")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (!Check(bus.SetChannelTopology(topology), "runtime attach from topology should succeed")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (!Check(bus.RegisterPublisher("broker", channel), "publisher registration should succeed")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (!Check(bus.Publish("broker", channel, std::vector<std::uint8_t>(30, 0x7f)), "publish within topology capacity should pass")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (!Check(bus.Publish("broker", channel, std::vector<std::uint8_t>(31, 0x7f)), "publish behavior remains valid after removing per-consumer depth scaling")) {
    shm_unlink(shm_name.c_str());
    return false;
  }

  shm_unlink(shm_name.c_str());
  return true;
}

bool TestBusRejectsPublishBeforeTopologyInit() {
  ShmBusRuntime bus;
  if (!Check(!bus.RegisterPublisher("broker", "broker.frames"), "register should fail before topology init")) {
    return false;
  }
  return Check(
      !bus.Publish("broker", "broker.frames", std::vector<std::uint8_t>(1, 0x01)),
      "publish should fail before topology init");
}

bool TestResolveShmRingConsumerCapacity() {
  if (!Check(ResolveShmRingConsumerCapacity(nullptr, 10) == 10, "null entry should use default floor")) {
    return false;
  }
  mould::config::ChannelTopologyEntry entry;
  entry.channel = "c";
  entry.consumer_count = 0;
  if (!Check(ResolveShmRingConsumerCapacity(&entry, 10) == 10, "zero consumer_count should use default")) {
    return false;
  }
  entry.consumer_count = 3;
  if (!Check(ResolveShmRingConsumerCapacity(&entry, 10) == 10, "topology below default should use default floor")) {
    return false;
  }
  entry.consumer_count = 20;
  return Check(
      ResolveShmRingConsumerCapacity(&entry, 10) == 20,
      "topology above default should use larger capacity");
}

bool TestCanonicalShmChannelKeyPrefersProducerThenConsumer() {
  mould::config::ChannelTopologyEntry producer_first;
  producer_first.channel = "events";
  producer_first.producers = {"broker"};
  producer_first.consumers = {"infer"};
  if (!Check(
          CanonicalShmChannelKey(producer_first) == "broker__events",
          "canonical key should prefer first producer")) {
    return false;
  }
  mould::config::ChannelTopologyEntry consumer_only;
  consumer_only.channel = "tasks";
  consumer_only.consumers = {"worker_a"};
  return Check(
      CanonicalShmChannelKey(consumer_only) == "worker_a__tasks",
      "canonical key should fall back to first consumer");
}

}  // namespace

int main(int argc, char* argv[]) {
  (void)argc;
  mould::InitApplicationLogging(argv[0]);
  bool ok = true;
  ok = TestParseModuleTxtConfig() && ok;
  ok = TestParseRejectsInvalidSyntax() && ok;
  ok = TestParseRejectsDuplicateOutputChannel() && ok;
  ok = TestBuildTopologyAndConflictChecks() && ok;
  ok = TestTopologyRejectsParameterConflict() && ok;
  ok = TestBuildTopologyFromConfigFiles() && ok;
  ok = TestShmPreallocationUsesTopology() && ok;
  ok = TestBusRejectsPublishBeforeTopologyInit() && ok;
  ok = TestResolveShmRingConsumerCapacity() && ok;
  ok = TestCanonicalShmChannelKeyPrefersProducerThenConsumer() && ok;

  if (!ok) {
    LOG(ERROR) << "module2 channel topology config tests failed";
    mould::ShutdownApplicationLogging();
    return 1;
  }

  LOG(INFO) << "module2 channel topology config tests passed";
  mould::ShutdownApplicationLogging();
  return 0;
}
