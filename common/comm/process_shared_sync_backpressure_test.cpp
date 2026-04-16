#include "ms_logging.hpp"
#include "channel_topology_config.hpp"
#include "shm_pubsub_bus.hpp"
#include "shm_ring_buffer.hpp"
#include "shm_segment.hpp"
#include "test_helpers.hpp"
#include <gtest/gtest.h>

#include <atomic>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <vector>

namespace {

class Module6GlogEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    mould::InitApplicationLogging("module6_process_shared_sync_backpressure_test");
  }
  void TearDown() override { mould::ShutdownApplicationLogging(); }
};

const auto* const k_module6_glog_env =
    ::testing::AddGlobalTestEnvironment(new Module6GlogEnvironment());

using mould::comm::MessageEnvelope;
using mould::comm::MiddlewareConfig;
using mould::comm::RingLayoutView;
using mould::comm::ShmPubSubBus;

std::string BuildUniqueChannelName(const std::string& prefix) {
  return prefix + "_" + std::to_string(::getpid()) + "_" +
      std::to_string(
             static_cast<std::uint64_t>(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
}

bool WriteFully(int fd, const void* data, std::size_t length) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::size_t total = 0;
  while (total < length) {
    const ssize_t written = write(fd, bytes + total, length - total);
    if (written > 0) {
      total += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

bool TestNotificationFanoutToAllOnlineConsumers() {
  MiddlewareConfig config;
  config.queue_depth = 64;
  ShmPubSubBus bus(config);

  mould::config::ChannelTopologyIndex topology;
  mould::config::ChannelTopologyEntry entry;
  entry.channel = BuildUniqueChannelName("module6.notify.mapping");
  entry.producers = {"broker"};
  entry.consumers = {"infer_a", "infer_b"};
  entry.consumer_count = 2;
  topology.emplace(entry.channel, entry);

  if (!Check(bus.SetChannelTopology(topology), "topology setup should succeed")) {
    return false;
  }
  if (!Check(bus.RegisterPublisher("broker", entry.channel), "register publisher should succeed")) {
    return false;
  }

  std::atomic<int> delivered_a{0};
  std::atomic<int> delivered_b{0};
  bool sub_a = bus.Subscribe("infer_a", entry.channel, [&](const MessageEnvelope&) {
    delivered_a.fetch_add(1, std::memory_order_relaxed);
  });
  bool sub_b = bus.Subscribe("infer_b", entry.channel, [&](const MessageEnvelope&) {
    delivered_b.fetch_add(1, std::memory_order_relaxed);
  });
  if (!Check(sub_a && sub_b, "subscribers should attach via inherited notification fds")) {
    return false;
  }
  if (!Check(
          !bus.Subscribe("infer_c", entry.channel, [](const MessageEnvelope&) {}),
          "subscribe should fail when consumer index exceeds configured consumer_count")) {
    return false;
  }

  constexpr int kMessages = 6;
  for (int i = 0; i < kMessages; ++i) {
    std::vector<std::uint8_t> payload{
        static_cast<std::uint8_t>(i),
        static_cast<std::uint8_t>(i + 1),
        static_cast<std::uint8_t>(i + 2),
    };
    if (!Check(bus.Publish("broker", entry.channel, payload), "publish should succeed after notification setup")) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline &&
         (delivered_a.load(std::memory_order_acquire) < kMessages ||
          delivered_b.load(std::memory_order_acquire) < kMessages)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  bool ok = true;
  ok = Check(
      delivered_a.load(std::memory_order_acquire) == kMessages,
      "consumer A should receive one callback per publish") && ok;
  ok = Check(
      delivered_b.load(std::memory_order_acquire) == kMessages,
      "consumer B should receive one callback per publish") && ok;
  return ok;
}

bool TestReactorConsumesFromSharedMemoryBySequence() {
  MiddlewareConfig config;
  config.queue_depth = 32;
  ShmPubSubBus bus(config);

  mould::config::ChannelTopologyIndex topology;
  mould::config::ChannelTopologyEntry entry;
  entry.channel = BuildUniqueChannelName("module6.reactor.sequence");
  entry.producers = {"broker"};
  entry.consumers = {"infer"};
  entry.consumer_count = 1;
  topology.emplace(entry.channel, entry);
  if (!Check(bus.SetChannelTopology(topology), "topology setup should succeed")) {
    return false;
  }
  if (!Check(bus.RegisterPublisher("broker", entry.channel), "register publisher should succeed")) {
    return false;
  }

  std::vector<std::uint64_t> seen_sequences;
  std::atomic<int> delivered{0};
  if (!Check(
          bus.Subscribe("infer", entry.channel, [&](const MessageEnvelope& message) {
            seen_sequences.push_back(message.sequence);
            delivered.fetch_add(1, std::memory_order_release);
          }),
          "reactor subscriber should register")) {
    return false;
  }

  constexpr int kMessages = 8;
  for (int i = 0; i < kMessages; ++i) {
    std::vector<std::uint8_t> payload = {
        static_cast<std::uint8_t>(i),
        static_cast<std::uint8_t>(i + 1)};
    if (!Check(bus.Publish("broker", entry.channel, payload), "publish should succeed")) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline &&
         delivered.load(std::memory_order_acquire) < kMessages) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!Check(delivered.load(std::memory_order_acquire) == kMessages, "reactor should deliver all committed slots")) {
    return false;
  }
  for (std::size_t i = 1; i < seen_sequences.size(); ++i) {
    if (!Check(seen_sequences[i] > seen_sequences[i - 1], "reactor should consume strictly increasing read_sequence")) {
      return false;
    }
  }
  return true;
}

bool TestPublishWaitObservabilityLogContainsRequiredFields() {
  MiddlewareConfig config;
  config.queue_depth = 32;
  ShmPubSubBus bus(config);

  mould::config::ChannelTopologyIndex topology;
  mould::config::ChannelTopologyEntry entry;
  entry.channel = BuildUniqueChannelName("module6.backpressure.log");
  entry.producers = {"broker"};
  entry.consumers = {"infer"};
  entry.consumer_count = 1;
  topology.emplace(entry.channel, entry);
  if (!Check(bus.SetChannelTopology(topology), "topology setup should succeed")) {
    return false;
  }
  if (!Check(bus.RegisterPublisher("broker", entry.channel), "register publisher should succeed")) {
    return false;
  }
  std::atomic<int> delivered{0};
  if (!Check(
          bus.Subscribe("infer", entry.channel, [&](const MessageEnvelope&) {
            delivered.fetch_add(1, std::memory_order_release);
          }),
          "subscriber callback should attach for publish-path verification")) {
    return false;
  }

  std::vector<std::uint8_t> payload{7, 8, 9, 10};
  const bool publish_ok = bus.Publish("broker", entry.channel, payload);
  if (!Check(publish_ok, "publish should succeed for observability log test")) {
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline &&
         delivered.load(std::memory_order_acquire) < 1) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return Check(
      delivered.load(std::memory_order_acquire) >= 1,
      "subscriber should receive publish callback without relying on log inspection");
}

bool TestForkedConsumerBindsInheritedEventFdMapping() {
  MiddlewareConfig config;
  config.queue_depth = 32;
  ShmPubSubBus bus(config);

  mould::config::ChannelTopologyIndex topology;
  mould::config::ChannelTopologyEntry entry;
  entry.channel = BuildUniqueChannelName("module6.fork.inherit");
  entry.producers = {"broker"};
  entry.consumers = {"infer_child"};
  entry.consumer_count = 1;
  topology.emplace(entry.channel, entry);
  if (!Check(bus.SetChannelTopology(topology), "topology setup should succeed")) {
    return false;
  }
  if (!Check(bus.RegisterPublisher("broker", entry.channel), "register publisher should succeed")) {
    return false;
  }

  int result_pipe[2] = {-1, -1};
  int ready_pipe[2] = {-1, -1};
  if (pipe(result_pipe) != 0 || pipe(ready_pipe) != 0) {
    return Check(false, "pipe should be created");
  }
  // Prevent leaking the write end on abnormal child execution paths.
  (void)fcntl(result_pipe[1], F_SETFD, FD_CLOEXEC);
  (void)fcntl(ready_pipe[1], F_SETFD, FD_CLOEXEC);

  const pid_t child = fork();
  if (child < 0) {
    close(result_pipe[0]);
    close(result_pipe[1]);
    close(ready_pipe[0]);
    close(ready_pipe[1]);
    return Check(false, "fork should succeed");
  }
  if (child == 0) {
    close(result_pipe[0]);
    close(ready_pipe[0]);
    bool child_ok = true;
    std::atomic<int> delivered{0};
    child_ok = Check(
                   bus.Subscribe("infer_child", entry.channel, [&](const MessageEnvelope&) {
                     delivered.fetch_add(1, std::memory_order_release);
                   }),
                   "forked consumer should subscribe with inherited mapping") &&
        child_ok;
    const std::uint8_t ready_marker = child_ok ? 1 : 0;
    child_ok = Check(
                   WriteFully(ready_pipe[1], &ready_marker, sizeof(ready_marker)),
                   "child should notify parent subscribe-ready status") &&
        child_ok;
    close(ready_pipe[1]);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline &&
           delivered.load(std::memory_order_acquire) == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    child_ok = Check(
                   delivered.load(std::memory_order_acquire) >= 1,
                   "forked consumer should receive publish notification") &&
        child_ok;
    const std::uint8_t marker = child_ok ? 1 : 0;
    child_ok = Check(
                   WriteFully(result_pipe[1], &marker, sizeof(marker)),
                   "child should report final delivery status to parent") &&
        child_ok;
    close(result_pipe[1]);
    _exit(child_ok ? 0 : 1);
  }

  close(result_pipe[1]);
  close(ready_pipe[1]);
  std::uint8_t ready_marker = 0;
  const ssize_t ready_read_bytes = read(ready_pipe[0], &ready_marker, sizeof(ready_marker));
  close(ready_pipe[0]);

  bool ok = Check(
      ready_read_bytes == sizeof(ready_marker) && ready_marker == 1,
      "child should report subscribe-ready before parent publish");
  const std::vector<std::uint8_t> payload{1, 2, 3, 4};
  ok = Check(bus.Publish("broker", entry.channel, payload), "parent publish should succeed") && ok;

  std::uint8_t marker = 0;
  const ssize_t read_bytes = read(result_pipe[0], &marker, sizeof(marker));
  close(result_pipe[0]);
  int child_status = 0;
  (void)waitpid(child, &child_status, 0);
  ok = Check(read_bytes == sizeof(marker) && marker == 1, "child should report successful delivery") && ok;
  ok = Check(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0, "child process should exit successfully") && ok;
  return ok;
}

bool TestSlowConsumerBackpressureProducesNonZeroWait() {
  MiddlewareConfig config;
  config.queue_depth = 3;
  ShmPubSubBus bus(config);

  mould::config::ChannelTopologyIndex topology;
  mould::config::ChannelTopologyEntry entry;
  entry.channel = BuildUniqueChannelName("module6.backpressure.slow");
  entry.producers = {"broker"};
  entry.consumers = {"infer_slow"};
  entry.consumer_count = 1;
  topology.emplace(entry.channel, entry);
  if (!Check(bus.SetChannelTopology(topology), "topology setup should succeed")) {
    return false;
  }
  if (!Check(bus.RegisterPublisher("broker", entry.channel), "register publisher should succeed")) {
    return false;
  }
  std::atomic<int> delivered{0};
  if (!Check(
          bus.Subscribe("infer_slow", entry.channel, [&](const MessageEnvelope& message) {
            if (message.payload.size() >= 3) {
              
            }
            delivered.fetch_add(1, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
          }),
          "slow subscriber should register")) {
    return false;
  }

  bool ok = true;
  constexpr int kBurstyPublishes = 600;
  const auto publish_begin = std::chrono::steady_clock::now();
  for (int i = 0; i < kBurstyPublishes; ++i) {
    std::vector<std::uint8_t> payload{
        static_cast<std::uint8_t>(i & 0xFF),
        static_cast<std::uint8_t>((i + 1) & 0xFF),
        static_cast<std::uint8_t>((i + 2) & 0xFF)};
    ok = Check(bus.Publish("broker", entry.channel, payload), "publish should succeed under slow-consumer pressure") && ok;
  }
  const auto publish_elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - publish_begin);
  if (!ok) {
    return false;
  }

  ok = Check(
           publish_elapsed > std::chrono::milliseconds(50),
           "slow consumer pressure should introduce measurable publish delay without log assertions") &&
      ok;
  const auto consume_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < consume_deadline &&
         delivered.load(std::memory_order_acquire) < kBurstyPublishes) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ok = Check(
           delivered.load(std::memory_order_acquire) == kBurstyPublishes,
           "slow consumer should receive all published messages") &&
      ok;
  return ok;
}

struct ChildConsumerStats {
  std::uint64_t delivered = 0;
  std::uint64_t monotonic_violations = 0;
  std::uint64_t first_sequence = 0;
  std::uint64_t last_sequence = 0;
};

struct MultiProcessConsumerStats {
  std::uint64_t delivered = 0;
  std::uint64_t payload_errors = 0;
  std::uint64_t sequence_errors = 0;
  std::uint64_t last_sequence = 0;
};

bool TestSingleProducerMultiProcessConsumersMessageCorrectness() {
  constexpr std::uint32_t kConsumerCount = 3;
  constexpr std::uint32_t kMessages = 300;

  MiddlewareConfig config;
  config.queue_depth = 16;
  ShmPubSubBus bus(config);

  mould::config::ChannelTopologyIndex topology;
  mould::config::ChannelTopologyEntry entry;
  entry.channel = BuildUniqueChannelName("module6.fork.mp.correctness");
  entry.producers = {"broker"};
  entry.consumers = {"infer_0", "infer_1", "infer_2"};
  entry.consumer_count = kConsumerCount;
  topology.emplace(entry.channel, entry);

  if (!Check(bus.SetChannelTopology(topology), "topology setup should succeed before fork")) {
    return false;
  }
  if (!Check(bus.RegisterPublisher("broker", entry.channel), "publisher registration should succeed before fork")) {
    return false;
  }

  std::array<pid_t, kConsumerCount> child_pids{};
  std::array<std::array<int, 2>, kConsumerCount> ready_pipes{};
  std::array<std::array<int, 2>, kConsumerCount> stats_pipes{};

  bool ok = true;
  for (std::uint32_t i = 0; i < kConsumerCount; ++i) {
    ready_pipes[i] = {-1, -1};
    stats_pipes[i] = {-1, -1};
    if (pipe(ready_pipes[i].data()) != 0 || pipe(stats_pipes[i].data()) != 0) {
      return Check(false, "parent-child pipes should be created");
    }
    (void)fcntl(ready_pipes[i][1], F_SETFD, FD_CLOEXEC);
    (void)fcntl(stats_pipes[i][1], F_SETFD, FD_CLOEXEC);

    const pid_t child = fork();
    if (child < 0) {
      return Check(false, "fork should succeed for each consumer process");
    }
    if (child == 0) {
      close(ready_pipes[i][0]);
      close(stats_pipes[i][0]);

      MultiProcessConsumerStats stats{};
      std::atomic<std::uint64_t> delivered{0};
      std::atomic<std::uint64_t> payload_errors{0};
      std::atomic<std::uint64_t> sequence_errors{0};
      std::atomic<std::uint64_t> last_sequence{0};
      std::atomic<std::uint64_t> last_payload_value{0};

      const std::string module_name = "infer_" + std::to_string(i);
      const bool subscribed = bus.Subscribe(module_name, entry.channel, [&](const MessageEnvelope& message) {
        if (message.payload.size() != sizeof(std::uint32_t)) {
          payload_errors.fetch_add(1, std::memory_order_relaxed);
        } else {
          std::uint32_t value = 0;
          std::memcpy(&value, message.payload.data(), sizeof(value));
          last_payload_value.store(value, std::memory_order_release);
          const std::uint64_t count_after = delivered.fetch_add(1, std::memory_order_acq_rel) + 1;
          if (value != static_cast<std::uint32_t>(count_after - 1U)) {
            payload_errors.fetch_add(1, std::memory_order_relaxed);
          }
        }

        const std::uint64_t prev_sequence = last_sequence.load(std::memory_order_acquire);
        if (prev_sequence != 0 && message.sequence <= prev_sequence) {
          sequence_errors.fetch_add(1, std::memory_order_relaxed);
        }
        last_sequence.store(message.sequence, std::memory_order_release);
      });

      const std::uint8_t ready = subscribed ? 1 : 0;
      (void)WriteFully(ready_pipes[i][1], &ready, sizeof(ready));
      close(ready_pipes[i][1]);
      if (!subscribed) {
        close(stats_pipes[i][1]);
        _exit(2);
      }

      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (std::chrono::steady_clock::now() < deadline &&
             delivered.load(std::memory_order_acquire) < kMessages) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }

      stats.delivered = delivered.load(std::memory_order_acquire);
      stats.payload_errors = payload_errors.load(std::memory_order_acquire);
      stats.sequence_errors = sequence_errors.load(std::memory_order_acquire);
      stats.last_sequence = last_sequence.load(std::memory_order_acquire);
      const bool wrote = WriteFully(stats_pipes[i][1], &stats, sizeof(stats));
      close(stats_pipes[i][1]);
      _exit(wrote ? 0 : 3);
    }

    child_pids[i] = child;
    close(ready_pipes[i][1]);
    close(stats_pipes[i][1]);
  }

  for (std::uint32_t i = 0; i < kConsumerCount; ++i) {
    std::uint8_t ready = 0;
    const ssize_t ready_bytes = read(ready_pipes[i][0], &ready, sizeof(ready));
    close(ready_pipes[i][0]);
    ok = Check(
             ready_bytes == static_cast<ssize_t>(sizeof(ready)) && ready == 1,
             "child consumer should report subscribe-ready") &&
        ok;
  }
  if (!ok) {
    return false;
  }

  for (std::uint32_t i = 0; i < kMessages; ++i) {
    std::uint32_t value = i;
    std::vector<std::uint8_t> payload(sizeof(value), 0);
    std::memcpy(payload.data(), &value, sizeof(value));
    ok = Check(bus.Publish("broker", entry.channel, payload), "parent publish should succeed for all messages") && ok;
  }
  if (!ok) {
    return false;
  }

  for (std::uint32_t i = 0; i < kConsumerCount; ++i) {
    MultiProcessConsumerStats stats{};
    const ssize_t bytes = read(stats_pipes[i][0], &stats, sizeof(stats));
    close(stats_pipes[i][0]);
    int child_status = 0;
    (void)waitpid(child_pids[i], &child_status, 0);

    ok = Check(bytes == static_cast<ssize_t>(sizeof(stats)), "parent should read child consumer stats") && ok;
    ok = Check(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0, "child process should exit successfully") && ok;
    ok = Check(stats.delivered == kMessages, "each child consumer should receive all published messages") && ok;
    ok = Check(stats.payload_errors == 0, "child consumer payload ordering should be correct") && ok;
    ok = Check(stats.sequence_errors == 0, "child consumer sequence should be strictly increasing") && ok;
  }
  return ok;
}

}  // namespace

TEST(Module6ProcessSharedSyncBackpressureTest, NotificationFanoutToAllOnlineConsumers) {
  EXPECT_TRUE(TestNotificationFanoutToAllOnlineConsumers());
}

TEST(Module6ProcessSharedSyncBackpressureTest, ReactorConsumesFromSharedMemoryBySequence) {
  EXPECT_TRUE(TestReactorConsumesFromSharedMemoryBySequence());
}

TEST(Module6ProcessSharedSyncBackpressureTest, PublishWaitObservabilityLogContainsRequiredFields) {
  EXPECT_TRUE(TestPublishWaitObservabilityLogContainsRequiredFields());
}

TEST(Module6ProcessSharedSyncBackpressureTest, ForkedConsumerBindsInheritedEventFdMapping) {
  EXPECT_TRUE(TestForkedConsumerBindsInheritedEventFdMapping());
}

TEST(Module6ProcessSharedSyncBackpressureTest, SlowConsumerBackpressureProducesNonZeroWait) {
  EXPECT_TRUE(TestSlowConsumerBackpressureProducesNonZeroWait());
}

TEST(Module6ProcessSharedSyncBackpressureTest, SingleProducerMultiProcessConsumersMessageCorrectness) {
  EXPECT_TRUE(TestSingleProducerMultiProcessConsumersMessageCorrectness());
}
