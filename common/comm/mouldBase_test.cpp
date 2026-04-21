#include "module_base.hpp"
#include "shm_bus_control_plane.hpp"
#include "shm_bus_runtime.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mould::comm {
namespace {

class TestModuleBase : public ModuleBase {
 public:
  TestModuleBase(
      std::string module_name,
      ModuleRuntimeContext runtime_context,
      bool do_init_result,
      std::unordered_map<std::string, IPubSubBus::MessageHandler> handlers,
      std::string publish_channel = {},
      int stop_after_iterations = 1)
      : ModuleBase(std::move(module_name), std::move(runtime_context)),
        do_init_result_(do_init_result),
        handlers_(std::move(handlers)),
        publish_channel_(std::move(publish_channel)),
        stop_after_iterations_(stop_after_iterations) {}

  bool do_init_called = false;
  bool setup_subscriptions_called = false;
  int loop_iterations = 0;
  bool publish_result_in_iteration = false;

 protected:
  bool DoInit() override {
    do_init_called = true;
    return do_init_result_;
  }

  bool SetupSubscriptions() override {
    setup_subscriptions_called = true;
    for (const auto& [channel, handler] : handlers_) {
      if (!SubscribeOneChannel(channel, handler)) {
        return false;
      }
    }
    return true;
  }

  void OnRunIteration() override {
    if (!publish_channel_.empty() && !published_once_) {
      publish_result_in_iteration = Publish(publish_channel_, ByteBuffer{1, 2, 3});
      published_once_ = true;
    }
    ++loop_iterations;
    if (loop_iterations >= stop_after_iterations_) {
      Stop();
    }
  }

 private:
  bool do_init_result_ = true;
  std::unordered_map<std::string, IPubSubBus::MessageHandler> handlers_;
  std::string publish_channel_;
  int stop_after_iterations_ = 1;
  bool published_once_ = false;
};

std::string WriteTempModuleChannelConfig(const std::string& contents) {
  const auto dir = std::filesystem::temp_directory_path();
  const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = dir / std::filesystem::path("mould_base_test_" + std::to_string(unique) + ".cfg");
  std::ofstream output(path);
  output << contents;
  output.close();
  return path.string();
}

class MouldBaseTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    setenv("MOULD_RELAX_FORK_ONLY_SHM_MODEL", "1", 1);
  }
};

TEST_F(MouldBaseTest, RunFailsWhenDoInitFails) {
  auto bus = std::make_shared<ShmBusRuntime>();
  ModuleRuntimeContext context;
  context.config.bus_kind = BusKind::kSingleNodeShm;
  context.shared_bus = bus;

  TestModuleBase module("mod_a", std::move(context), false, {});
  EXPECT_FALSE(module.Run());
  EXPECT_TRUE(module.do_init_called);
  EXPECT_FALSE(module.setup_subscriptions_called);
}

TEST_F(MouldBaseTest, SubscribeUsesConfiguredInputChannelsEvenWithoutUserHandler) {
  auto bus = std::make_shared<ShmBusRuntime>();
  ShmBusControlPlane control_plane;

  const std::string self_cfg_path = WriteTempModuleChannelConfig(
      "input ch_a\n"
      "input ch_b\n"
      "output ch_out\n");
  const std::string pub_cfg_path = WriteTempModuleChannelConfig("output ch_a\noutput ch_b\n");
  std::vector<std::pair<std::string, std::string>> module_cfgs{
      {"mod_b", self_cfg_path},
      {"publisher", pub_cfg_path},
  };
  std::string provision_error;
  ASSERT_TRUE(control_plane.ProvisionChannelTopologyFromModuleConfigs(module_cfgs, &provision_error));

  ModuleRuntimeContext context;
  context.config.bus_kind = BusKind::kSingleNodeShm;
  context.config.module_channel_config_path = self_cfg_path;
  context.config.module_config_files = module_cfgs;
  context.shared_bus = bus;

  TestModuleBase module("mod_b", std::move(context), true, {});
  EXPECT_TRUE(module.Run());
  EXPECT_TRUE(module.do_init_called);
  EXPECT_TRUE(module.setup_subscriptions_called);
  EXPECT_EQ(module.loop_iterations, 1);

  std::error_code ec;
  std::filesystem::remove(self_cfg_path, ec);
  std::filesystem::remove(pub_cfg_path, ec);
  control_plane.FinalizeUnlinkManagedSegments();
}

TEST_F(MouldBaseTest, SubscriptionHandlerFromBusinessIsInvoked) {
  auto bus = std::make_shared<ShmBusRuntime>();
  ShmBusControlPlane control_plane;

  const std::string self_cfg_path = WriteTempModuleChannelConfig(
      "input ch_business\n"
      "output ch_business\n");
  std::vector<std::pair<std::string, std::string>> module_cfgs{{"mod_c", self_cfg_path}};
  std::string provision_error;
  ASSERT_TRUE(control_plane.ProvisionChannelTopologyFromModuleConfigs(module_cfgs, &provision_error));
  std::atomic<int> callback_count{0};
  std::unordered_map<std::string, IPubSubBus::MessageHandler> handlers;
  handlers.emplace("ch_business", [&callback_count](const MessageEnvelope&) { ++callback_count; });

  ModuleRuntimeContext context;
  context.config.bus_kind = BusKind::kSingleNodeShm;
  context.config.module_channel_config_path = self_cfg_path;
  context.config.module_config_files = module_cfgs;
  context.shared_bus = bus;

  TestModuleBase module(
      "mod_c", std::move(context), true, std::move(handlers), "ch_business", 20);
  EXPECT_TRUE(module.Run());
  EXPECT_GT(callback_count.load(), 0);

  std::error_code ec;
  std::filesystem::remove(self_cfg_path, ec);
  control_plane.FinalizeUnlinkManagedSegments();
}

TEST_F(MouldBaseTest, RunFailsWhenModuleChannelConfigFileMissing) {
  auto bus = std::make_shared<ShmBusRuntime>();
  ModuleRuntimeContext context;
  context.config.bus_kind = BusKind::kSingleNodeShm;
  context.config.module_channel_config_path = "/tmp/not_exists_mould_base.cfg";
  context.shared_bus = bus;

  TestModuleBase module("mod_d", std::move(context), true, {});
  EXPECT_FALSE(module.Run());
  EXPECT_TRUE(module.do_init_called);
  EXPECT_TRUE(module.setup_subscriptions_called);
}

TEST_F(MouldBaseTest, RunFailsWhenSubscribeWithoutTopologyProvision) {
  auto bus = std::make_shared<ShmBusRuntime>();

  const std::string cfg_path = WriteTempModuleChannelConfig("input ch_sub_fail\n");
  ModuleRuntimeContext context;
  context.config.bus_kind = BusKind::kSingleNodeShm;
  context.config.module_channel_config_path = cfg_path;
  context.shared_bus = bus;

  TestModuleBase module("mod_e", std::move(context), true, {});
  EXPECT_FALSE(module.Run());

  std::error_code ec;
  std::filesystem::remove(cfg_path, ec);
}

TEST_F(MouldBaseTest, RunSucceedsWithoutChannelConfigPath) {
  auto bus = std::make_shared<ShmBusRuntime>();
  ModuleRuntimeContext context;
  context.config.bus_kind = BusKind::kSingleNodeShm;
  context.shared_bus = bus;

  TestModuleBase module("mod_f", std::move(context), true, {});
  EXPECT_TRUE(module.Run());
  EXPECT_EQ(module.loop_iterations, 1);
}

TEST_F(MouldBaseTest, RunSucceedsWhenConfigHasOnlyOutputChannels) {
  auto bus = std::make_shared<ShmBusRuntime>();
  ShmBusControlPlane control_plane;
  const std::string cfg_path = WriteTempModuleChannelConfig(
      "output ch_out_a\n"
      "output ch_out_b\n");
  std::vector<std::pair<std::string, std::string>> module_cfgs{{"mod_g", cfg_path}};
  std::string provision_error;
  ASSERT_TRUE(control_plane.ProvisionChannelTopologyFromModuleConfigs(module_cfgs, &provision_error));

  ModuleRuntimeContext context;
  context.config.bus_kind = BusKind::kSingleNodeShm;
  context.config.module_channel_config_path = cfg_path;
  context.config.module_config_files = module_cfgs;
  context.shared_bus = bus;

  TestModuleBase module("mod_g", std::move(context), true, {});
  EXPECT_TRUE(module.Run());

  std::error_code ec;
  std::filesystem::remove(cfg_path, ec);
  control_plane.FinalizeUnlinkManagedSegments();
}

TEST_F(MouldBaseTest, UnmatchedBusinessHandlerUsesDefaultNoopHandler) {
  auto bus = std::make_shared<ShmBusRuntime>();
  ShmBusControlPlane control_plane;

  const std::string cfg_path = WriteTempModuleChannelConfig(
      "input ch_need_default\n"
      "output ch_need_default\n");
  std::vector<std::pair<std::string, std::string>> module_cfgs{{"mod_h", cfg_path}};
  std::string provision_error;
  ASSERT_TRUE(control_plane.ProvisionChannelTopologyFromModuleConfigs(module_cfgs, &provision_error));
  std::atomic<int> callback_count{0};
  std::unordered_map<std::string, IPubSubBus::MessageHandler> handlers;
  handlers.emplace("ch_other", [&callback_count](const MessageEnvelope&) { ++callback_count; });

  ModuleRuntimeContext context;
  context.config.bus_kind = BusKind::kSingleNodeShm;
  context.config.module_channel_config_path = cfg_path;
  context.config.module_config_files = module_cfgs;
  context.shared_bus = bus;

  TestModuleBase module(
      "mod_h", std::move(context), true, std::move(handlers), "ch_need_default", 20);
  EXPECT_TRUE(module.Run());
  EXPECT_EQ(callback_count.load(), 0);

  std::error_code ec;
  std::filesystem::remove(cfg_path, ec);
  control_plane.FinalizeUnlinkManagedSegments();
}

TEST_F(MouldBaseTest, DoInitAndSetupSubscriptionsAreCalledOnSuccessfulRun) {
  auto bus = std::make_shared<ShmBusRuntime>();
  ShmBusControlPlane control_plane;
  const std::string cfg_path = WriteTempModuleChannelConfig(
      "input ch_x\n"
      "input ch_y\n");
  const std::string pub_cfg_path = WriteTempModuleChannelConfig("output ch_x\noutput ch_y\n");
  std::vector<std::pair<std::string, std::string>> module_cfgs{
      {"mod_i", cfg_path},
      {"publisher_i", pub_cfg_path},
  };
  std::string provision_error;
  ASSERT_TRUE(control_plane.ProvisionChannelTopologyFromModuleConfigs(module_cfgs, &provision_error));

  ModuleRuntimeContext context;
  context.config.bus_kind = BusKind::kSingleNodeShm;
  context.config.module_channel_config_path = cfg_path;
  context.config.module_config_files = module_cfgs;
  context.shared_bus = bus;

  TestModuleBase module("mod_i", std::move(context), true, {});
  EXPECT_TRUE(module.Run());
  EXPECT_TRUE(module.do_init_called);
  EXPECT_TRUE(module.setup_subscriptions_called);

  std::error_code ec;
  std::filesystem::remove(cfg_path, ec);
  std::filesystem::remove(pub_cfg_path, ec);
  control_plane.FinalizeUnlinkManagedSegments();
}

TEST_F(MouldBaseTest, PublishAllowedOnlyDuringRunningStage) {
  auto bus = std::make_shared<ShmBusRuntime>();
  ShmBusControlPlane control_plane;
  const std::string cfg_path = WriteTempModuleChannelConfig("output ch_publish\n");
  std::vector<std::pair<std::string, std::string>> module_cfgs{{"mod_j", cfg_path}};
  std::string provision_error;
  ASSERT_TRUE(control_plane.ProvisionChannelTopologyFromModuleConfigs(module_cfgs, &provision_error));

  ModuleRuntimeContext context;
  context.config.bus_kind = BusKind::kSingleNodeShm;
  context.config.module_channel_config_path = cfg_path;
  context.config.module_config_files = module_cfgs;
  context.shared_bus = bus;

  TestModuleBase module("mod_j", std::move(context), true, {}, "ch_publish", 1);
  EXPECT_TRUE(module.Run());
  EXPECT_TRUE(module.publish_result_in_iteration);

  std::error_code ec;
  std::filesystem::remove(cfg_path, ec);
  control_plane.FinalizeUnlinkManagedSegments();
}

}  // namespace
}  // namespace mould::comm
