#include "ready_pipe_protocol.hpp"
#include "supervisor.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>
#include <vector>

namespace mould::comm {
namespace {

TEST(SupervisorSpec, Supervisor_StartupPriorityBatchReadyBarrier) {
  Supervisor supervisor(/*random_seed=*/7U);
  const std::vector<SupervisorModuleSpec> modules = {
      {"mod_a", 20},
      {"mod_b", 10},
      {"mod_c", 20},
      {"mod_d", 30},
      {"mod_e", 10},
  };

  std::string error;
  ASSERT_TRUE(supervisor.ValidateSingleModulePerProcessInvariant(modules, &error)) << error;

  const auto batches = supervisor.BuildInitialStartupBatches(modules);
  ASSERT_EQ(batches.size(), 3U);
  EXPECT_EQ(batches[0].front().startup_priority, 10);
  EXPECT_EQ(batches[1].front().startup_priority, 20);
  EXPECT_EQ(batches[2].front().startup_priority, 30);
  EXPECT_EQ(batches[0].size(), 2U);
  EXPECT_EQ(batches[1].size(), 2U);
  EXPECT_EQ(batches[2].size(), 1U);
}

TEST(SupervisorSpec, Supervisor_SamePriorityRandomizationRules) {
  Supervisor supervisor(/*random_seed=*/19U);

  const std::vector<SupervisorModuleSpec> single = {
      {"mod_one", 20},
  };
  const auto single_ordered = supervisor.OrderSamePriorityRestartBatch(single);
  ASSERT_EQ(single_ordered.size(), 1U);
  EXPECT_EQ(single_ordered[0].module_name, "mod_one");

  const std::vector<SupervisorModuleSpec> multi = {
      {"mod_a", 20},
      {"mod_b", 20},
      {"mod_c", 20},
      {"mod_d", 20},
  };
  const auto multi_ordered = supervisor.OrderSamePriorityRestartBatch(multi);
  ASSERT_EQ(multi_ordered.size(), multi.size());

  std::unordered_set<std::string> original_names;
  for (const auto& module : multi) {
    original_names.insert(module.module_name);
  }
  std::unordered_set<std::string> reordered_names;
  for (const auto& module : multi_ordered) {
    reordered_names.insert(module.module_name);
  }
  EXPECT_EQ(original_names, reordered_names);

  bool same_order = true;
  for (std::size_t i = 0; i < multi.size(); ++i) {
    if (multi[i].module_name != multi_ordered[i].module_name) {
      same_order = false;
      break;
    }
  }
  EXPECT_FALSE(same_order);
}

TEST(SupervisorTest, ForkOnlySingleModuleProcessLifecycle) {
  Supervisor supervisor(/*random_seed=*/13U);
  std::string error;

  ASSERT_TRUE(supervisor.ForkModuleProcess("worker_alpha", []() { return 0; }, &error)) << error;
  ASSERT_TRUE(supervisor.HasChildProcess("worker_alpha"));
  const auto child_pid = supervisor.ChildPidOf("worker_alpha");
  ASSERT_TRUE(child_pid.has_value());

  int status = 0;
  ASSERT_EQ(waitpid(*child_pid, &status, 0), *child_pid);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);
  EXPECT_TRUE(supervisor.ReapChildProcess(*child_pid));
  EXPECT_FALSE(supervisor.HasChildProcess("worker_alpha"));

  ASSERT_TRUE(supervisor.ForkModuleProcess("worker_alpha", []() { return 0; }, &error)) << error;
  const auto replacement_pid = supervisor.ChildPidOf("worker_alpha");
  ASSERT_TRUE(replacement_pid.has_value());
  ASSERT_NE(*replacement_pid, *child_pid);
  ASSERT_EQ(waitpid(*replacement_pid, &status, 0), *replacement_pid);
  EXPECT_TRUE(supervisor.ReapChildProcess(*replacement_pid));
}

TEST(SupervisorTest, RejectsDuplicateModulesForSingleInstanceInvariant) {
  Supervisor supervisor;
  const std::vector<SupervisorModuleSpec> modules = {
      {"mod_dup", 10},
      {"mod_dup", 20},
  };
  std::string error;
  EXPECT_FALSE(supervisor.ValidateSingleModulePerProcessInvariant(modules, &error));
  EXPECT_NE(error.find("duplicate module"), std::string::npos);
}

TEST(ReadyPipeProtocolSpec, ReadyPipeProtocol_TransitionsSuccessTimeoutFailure) {
  Supervisor supervisor(/*random_seed=*/23U);
  int parent_read_fd = -1;
  int child_write_fd = -1;
  std::string error;
  ASSERT_TRUE(ReadyPipeProtocol::CreateParentManagedPipe(&parent_read_fd, &child_write_fd, &error)) << error;

  supervisor.TransitionToForked("mod_ready_success");
  ASSERT_TRUE(ReadyPipeProtocol::SendReady(child_write_fd, &error)) << error;
  EXPECT_TRUE(supervisor.WaitForReadyOrTransitionFailed("mod_ready_success", parent_read_fd, 50));
  const auto ready_state = supervisor.LifecycleOf("mod_ready_success");
  ASSERT_TRUE(ready_state.has_value());
  EXPECT_EQ(ready_state->state, ModuleLifecycleState::kReady);
  EXPECT_TRUE(supervisor.CanReleaseNextPriorityBatch({"mod_ready_success"}));
  ReadyPipeProtocol::CloseFdIfOpen(&child_write_fd);
  ReadyPipeProtocol::CloseFdIfOpen(&parent_read_fd);

  ASSERT_TRUE(ReadyPipeProtocol::CreateParentManagedPipe(&parent_read_fd, &child_write_fd, &error)) << error;
  supervisor.TransitionToForked("mod_ready_timeout");
  EXPECT_FALSE(supervisor.WaitForReadyOrTransitionFailed("mod_ready_timeout", parent_read_fd, 1));
  const auto timeout_state = supervisor.LifecycleOf("mod_ready_timeout");
  ASSERT_TRUE(timeout_state.has_value());
  EXPECT_EQ(timeout_state->state, ModuleLifecycleState::kFailed);
  EXPECT_NE(timeout_state->failure_reason.find("timeout"), std::string::npos);
  EXPECT_TRUE(supervisor.CanReleaseNextPriorityBatch({"mod_ready_timeout"}));
  ReadyPipeProtocol::CloseFdIfOpen(&child_write_fd);
  ReadyPipeProtocol::CloseFdIfOpen(&parent_read_fd);

  supervisor.TransitionToIniting("mod_not_ready");
  EXPECT_FALSE(supervisor.CanReleaseNextPriorityBatch({"mod_not_ready"}));
}

TEST(RestartPolicySpec, RestartPolicy_BackoffRetryWindowFuseBehavior) {
  Supervisor supervisor(/*random_seed=*/37U);
  const RestartPolicyConfig cfg{
      .restart_backoff_ms = 100,
      .restart_max_retries = 3,
      .restart_window_ms = 1000,
      .restart_fuse_ms = 5000,
  };

  const auto d1 = supervisor.HandleAbnormalChildExit("mod_restart", cfg, 1000);
  EXPECT_TRUE(d1.should_restart);
  EXPECT_EQ(d1.restart_delay_ms, 100);
  EXPECT_FALSE(d1.fuse_open);

  const auto d2 = supervisor.HandleAbnormalChildExit("mod_restart", cfg, 1100);
  EXPECT_TRUE(d2.should_restart);
  EXPECT_EQ(d2.restart_delay_ms, 200);
  EXPECT_FALSE(d2.fuse_open);

  const auto d3 = supervisor.HandleAbnormalChildExit("mod_restart", cfg, 1200);
  EXPECT_TRUE(d3.should_restart);
  EXPECT_EQ(d3.restart_delay_ms, 400);
  EXPECT_FALSE(d3.fuse_open);

  const auto d4 = supervisor.HandleAbnormalChildExit("mod_restart", cfg, 1300);
  EXPECT_FALSE(d4.should_restart);
  EXPECT_TRUE(d4.fuse_open);

  const auto d5 = supervisor.HandleAbnormalChildExit("mod_restart", cfg, 2000);
  EXPECT_FALSE(d5.should_restart);
  EXPECT_TRUE(d5.fuse_open);

  const auto d6 = supervisor.HandleAbnormalChildExit("mod_restart", cfg, 7001);
  EXPECT_TRUE(d6.should_restart);
  EXPECT_FALSE(d6.fuse_open);
}

TEST(SupervisorSpec, Supervisor_FuseOpenKeepsMasterAlive) {
  Supervisor supervisor(/*random_seed=*/41U);
  const RestartPolicyConfig cfg{
      .restart_backoff_ms = 50,
      .restart_max_retries = 1,
      .restart_window_ms = 1000,
      .restart_fuse_ms = 10000,
  };
  (void)supervisor.HandleAbnormalChildExit("mod_unstable", cfg, 100);
  const auto fuse = supervisor.HandleAbnormalChildExit("mod_unstable", cfg, 200);
  EXPECT_TRUE(fuse.fuse_open);
  EXPECT_FALSE(fuse.should_restart);
  EXPECT_TRUE(supervisor.IsMasterAlive());
}

TEST(SupervisorIntegrationSpec, MultiPriorityStartupReadyBarrierFlow) {
  Supervisor supervisor(/*random_seed=*/51U);
  const std::vector<SupervisorModuleSpec> modules = {
      {"p10_a", 10},
      {"p10_b", 10},
      {"p20_a", 20},
  };
  const auto batches = supervisor.BuildInitialStartupBatches(modules);
  ASSERT_EQ(batches.size(), 2U);
  ASSERT_EQ(batches[0].size(), 2U);
  ASSERT_EQ(batches[1].size(), 1U);

  std::vector<std::string> first_batch;
  for (const auto& spec : batches[0]) {
    first_batch.push_back(spec.module_name);
    supervisor.TransitionToForked(spec.module_name);
    supervisor.TransitionToIniting(spec.module_name);
  }
  EXPECT_FALSE(supervisor.CanReleaseNextPriorityBatch(first_batch));

  supervisor.TransitionToReady(first_batch[0]);
  EXPECT_FALSE(supervisor.CanReleaseNextPriorityBatch(first_batch));
  supervisor.TransitionToFailed(first_batch[1], "ready timeout");
  EXPECT_TRUE(supervisor.CanReleaseNextPriorityBatch(first_batch));
}

TEST(SupervisorIntegrationSpec, ObservabilityTracksReadyRestartFuseAndOrder) {
  Supervisor supervisor(/*random_seed=*/57U);
  const std::vector<SupervisorModuleSpec> modules = {
      {"trace_a", 10},
      {"trace_b", 10},
  };
  (void)supervisor.BuildInitialStartupBatches(modules);
  (void)supervisor.OrderSamePriorityRestartBatch(modules);
  supervisor.TransitionToReady("trace_a");

  const RestartPolicyConfig cfg{
      .restart_backoff_ms = 10,
      .restart_max_retries = 1,
      .restart_window_ms = 1000,
      .restart_fuse_ms = 1000,
  };
  (void)supervisor.HandleAbnormalChildExit("trace_a", cfg, 10);
  (void)supervisor.HandleAbnormalChildExit("trace_a", cfg, 20);

  const auto metrics = supervisor.ObservabilitySnapshot();
  ASSERT_TRUE(metrics.ready_transitions.contains("trace_a"));
  EXPECT_EQ(metrics.ready_transitions.at("trace_a"), 1U);
  ASSERT_TRUE(metrics.restart_evaluations.contains("trace_a"));
  EXPECT_EQ(metrics.restart_evaluations.at("trace_a"), 2U);
  ASSERT_TRUE(metrics.fuse_open_events.contains("trace_a"));
  EXPECT_EQ(metrics.fuse_open_events.at("trace_a"), 1U);
  EXPECT_EQ(metrics.startup_order_trace.size(), 2U);
  EXPECT_EQ(metrics.restart_order_trace.size(), 2U);
}

TEST(SupervisorGtest, MainSupervisor_StartupByPriority_RespectsReadyBarrier) {
  Supervisor supervisor(/*random_seed=*/61U);
  const std::vector<SupervisorModuleSpec> modules = {
      {"p10_a", 10},
      {"p10_b", 10},
      {"p20_a", 20},
  };
  const auto batches = supervisor.BuildInitialStartupBatches(modules);
  ASSERT_EQ(batches.size(), 2U);
  std::vector<std::string> first_batch_modules;
  for (const auto& spec : batches[0]) {
    first_batch_modules.push_back(spec.module_name);
    supervisor.TransitionToIniting(spec.module_name);
  }
  EXPECT_FALSE(supervisor.CanReleaseNextPriorityBatch(first_batch_modules));
  supervisor.TransitionToReady(first_batch_modules.front());
  EXPECT_FALSE(supervisor.CanReleaseNextPriorityBatch(first_batch_modules));
  supervisor.TransitionToFailed(first_batch_modules.back(), "ready timeout");
  EXPECT_TRUE(supervisor.CanReleaseNextPriorityBatch(first_batch_modules));
}

TEST(SupervisorGtest, MainSupervisor_RestartPolicyOnCrash_PerformsBackoffOrFuse) {
  Supervisor supervisor(/*random_seed=*/67U);
  const RestartPolicyConfig cfg{
      .restart_backoff_ms = 10,
      .restart_max_retries = 2,
      .restart_window_ms = 200,
      .restart_fuse_ms = 1000,
  };
  const auto first = supervisor.HandleAbnormalChildExit("unstable", cfg, 1000);
  EXPECT_TRUE(first.should_restart);
  EXPECT_FALSE(first.fuse_open);
  const auto second = supervisor.HandleAbnormalChildExit("unstable", cfg, 1100);
  EXPECT_TRUE(second.should_restart);
  EXPECT_FALSE(second.fuse_open);
  const auto third = supervisor.HandleAbnormalChildExit("unstable", cfg, 1200);
  EXPECT_FALSE(third.should_restart);
  EXPECT_TRUE(third.fuse_open);
}

TEST(SupervisorGtest, MainSupervisor_Shutdown_ReapsChildrenWithoutZombie) {
  Supervisor supervisor(/*random_seed=*/71U);
  std::string error;
  ASSERT_TRUE(supervisor.ForkModuleProcess("reap_one", []() { return 0; }, &error)) << error;
  ASSERT_TRUE(supervisor.ForkModuleProcess("reap_two", []() { return 0; }, &error)) << error;

  auto pid1 = supervisor.ChildPidOf("reap_one");
  auto pid2 = supervisor.ChildPidOf("reap_two");
  ASSERT_TRUE(pid1.has_value());
  ASSERT_TRUE(pid2.has_value());

  int status = 0;
  ASSERT_EQ(waitpid(*pid1, &status, 0), *pid1);
  ASSERT_EQ(waitpid(*pid2, &status, 0), *pid2);
  EXPECT_TRUE(supervisor.ReapChildProcess(*pid1));
  EXPECT_TRUE(supervisor.ReapChildProcess(*pid2));
  EXPECT_FALSE(supervisor.HasChildProcess("reap_one"));
  EXPECT_FALSE(supervisor.HasChildProcess("reap_two"));
}

}  // namespace
}  // namespace mould::comm
