#include "ms_logging.hpp"
#include "channel_topology_config.hpp"
#include "launch_plan_config.hpp"
#include "shm_bus_control_plane.hpp"
#include "shm_bus_runtime.hpp"
#include "shm_bus_runtime_prefork.hpp"
#include "shm_ring_buffer.hpp"
#include "test_helpers.hpp"
#include <gtest/gtest.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

class SplitShmBusControlRuntimeGlogEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    mould::InitApplicationLogging("split_shm_bus_control_runtime_test");
  }
  void TearDown() override { mould::ShutdownApplicationLogging(); }
};

const auto* const k_split_shm_bus_control_runtime_glog_env =
    ::testing::AddGlobalTestEnvironment(new SplitShmBusControlRuntimeGlogEnvironment());

namespace fs = std::filesystem;

bool WriteFully(int fd, const void* data, std::size_t length) {
  const auto* p = static_cast<const std::uint8_t*>(data);
  std::size_t off = 0;
  while (off < length) {
    const ssize_t n = ::write(fd, p + off, length - off);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (n == 0) {
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  return true;
}

bool ReadFully(int fd, void* data, std::size_t length) {
  auto* p = static_cast<std::uint8_t*>(data);
  std::size_t off = 0;
  while (off < length) {
    const ssize_t n = ::read(fd, p + off, length - off);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (n == 0) {
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  return true;
}

bool WaitAtomicUintAtLeast(const std::atomic<std::uint32_t>* c, std::uint32_t want, int timeout_ms = 12000) {
  using clock = std::chrono::steady_clock;
  const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
  while (clock::now() < deadline) {
    if (c->load(std::memory_order_acquire) >= want) {
      return true;
    }
    usleep(500);
  }
  return false;
}

bool WaitAtomicRefUintAtLeast(std::uint32_t* word, std::uint32_t want, int timeout_ms = 180000) {
  std::atomic_ref<std::uint32_t> r(*word);
  using clock = std::chrono::steady_clock;
  const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
  std::uint32_t last_reported = 0;
  int report_counter = 0;

  while (clock::now() < deadline) {
    std::uint32_t current = r.load(std::memory_order_acquire);

    // 每隔一段时间打印一次当前消费进度（便于观察）
    if (current != last_reported && (report_counter % 50 == 0 || current >= want)) {
      LOG(INFO) << "WaitAtomicRefUintAtLeast: current=" << current
                << " want=" << want << " remaining_ms≈"
                << std::chrono::duration_cast<std::chrono::milliseconds>(
                       deadline - clock::now()).count();
      last_reported = current;
    }
    report_counter++;

    usleep(200);
  }

  // 必须等到超时后才返回最终结果
  const std::uint32_t final = r.load(std::memory_order_acquire);
  if (final != want) {
    LOG(ERROR) << "WaitAtomicRefUintAtLeast timeout. Final delivered = " << final
               << ", expected exactly " << want << " (strict wait until deadline)";
  } else {
    LOG(INFO) << "WaitAtomicRefUintAtLeast success. Final delivered = " << final
              << " (exactly matched after full timeout wait)";
  }

  return final == want;
}

using mould::comm::ShmBusRuntimeFinalizeSharedControlPlaneShm;
using mould::comm::ShmBusRuntimeGetSharedControlPlane;
using mould::comm::ShmBusRuntimePreforkEnsureSharedControlPlane;

bool ShmPosixObjectExists(const std::string& posix_shm_name) {
  const int fd = shm_open(posix_shm_name.c_str(), O_RDONLY, 0);
  if (fd >= 0) {
    close(fd);
    return true;
  }
  return false;
}

fs::path MakeUniqueRoot() {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<std::uint64_t> dist;
  return fs::temp_directory_path() / ("mould_split_shm_ut_" + std::to_string(dist(gen)));
}

void WriteFile(const fs::path& path, const std::string& content) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  out << content;
}

bool TestShmUnlinkPolicy_RuntimeVsFinalExit() {
  ShmBusRuntimeFinalizeSharedControlPlaneShm();
  ShmBusRuntimePreforkEnsureSharedControlPlane();
  auto plane = ShmBusRuntimeGetSharedControlPlane();
  if (!Check(static_cast<bool>(plane), "shared control plane should exist")) {
    return false;
  }

  mould::comm::MiddlewareConfig cfg;
  const std::string channel =
      "split_ut_ch_" + std::to_string(getpid()) + "_" +
      std::to_string(
          static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));

  mould::config::ChannelTopologyIndex topology;
  mould::config::ChannelTopologyEntry entry;
  entry.channel = channel;
  entry.producers = {"broker"};
  entry.consumers = {"infer"};
  entry.consumer_count = 1;
  topology.emplace(channel, entry);

  const std::string posix = mould::comm::BuildDeterministicShmName(
      mould::config::CanonicalShmChannelKey(entry));
  (void)shm_unlink(posix.c_str());

  {
    if (!Check(plane->ProvisionChannelTopology(topology, cfg), "supervisor should provision shm")) {
      return false;
    }
  }
  {
    mould::comm::ShmBusRuntime bus(cfg);
    if (!Check(bus.SetChannelTopology(topology), "runtime should attach to provisioned shm")) {
      return false;
    }
    if (!Check(bus.RegisterPublisher("broker", channel), "publisher register")) {
      return false;
    }
    if (!Check(
            bus.Publish("broker", channel, std::vector<std::uint8_t>{0x01, 0x02}),
            "publish should succeed")) {
      return false;
    }
    if (!Check(plane->FinalizeUnlinkInvocationCount() == 0, "no final unlink during runtime")) {
      return false;
    }
    if (!Check(ShmPosixObjectExists(posix), "posix shm object should exist while bus is alive")) {
      return false;
    }
  }

  if (!Check(ShmPosixObjectExists(posix), "shm should remain after bus teardown (no auto unlink)")) {
    return false;
  }
  ShmBusRuntimeFinalizeSharedControlPlaneShm();
  if (!Check(!ShmPosixObjectExists(posix), "final exit path should unlink managed shm")) {
    return false;
  }
  return Check(plane->FinalizeUnlinkInvocationCount() >= 1, "finalize unlink should have run");
}

bool TestRegistryFrozen_RejectsUnknownChannelKey() {
  mould::comm::MiddlewareConfig cfg;
  mould::comm::ShmBusControlPlane control_plane;
  mould::comm::ShmBusRuntime bus(cfg);
  const std::string channel =
      "split_ut_frz_" + std::to_string(getpid()) + "_" +
      std::to_string(
          static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));

  mould::config::ChannelTopologyIndex topology;
  mould::config::ChannelTopologyEntry entry;
  entry.channel = channel;
  entry.producers = {"broker"};
  entry.consumers = {"infer"};
  entry.consumer_count = 1;
  topology.emplace(channel, entry);

  if (!Check(control_plane.ProvisionChannelTopology(topology, cfg), "topology init")) {
    return false;
  }
  if (!Check(bus.SetChannelTopology(topology), "runtime attach")) {
    return false;
  }
  return Check(
      !bus.RegisterPublisher("broker", channel + "_unknown_suffix"),
      "unknown channel should fail after registry freeze");
}

bool TestCompeteDeliveryMode_EndToEndSingleHandlerPerPublish() {
  mould::comm::MiddlewareConfig config;
  config.slot_payload_bytes = 64;
  config.backlog_alarm_threshold = config.slot_payload_bytes;
  config.default_consumer_slots_per_channel = 2;
  mould::comm::ShmBusControlPlane control_plane;
  const std::string channel =
      "split_ut_compete_" + std::to_string(getpid()) + "_" +
      std::to_string(
          static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));

  mould::config::ChannelTopologyIndex topology;
  mould::config::ChannelTopologyEntry entry;
  entry.channel = channel;
  entry.producers = {"broker"};
  entry.consumers = {"infer_a", "infer_b"};
  entry.consumer_count = 2;
  entry.params["delivery_mode"] = "compete";
  topology.emplace(entry.channel, entry);

  const std::string posix_name =
      mould::comm::BuildDeterministicShmName(mould::config::CanonicalShmChannelKey(entry));
  (void)shm_unlink(posix_name.c_str());

  if (!Check(control_plane.ProvisionChannelTopology(topology, config), "provision compete channel")) {
    return false;
  }
  mould::comm::ShmBusRuntime bus(config);
  if (!Check(bus.SetChannelTopology(topology), "runtime attach compete")) {
    return false;
  }
  if (!Check(bus.RegisterPublisher("broker", entry.channel), "publisher")) {
    return false;
  }
  std::atomic<int> delivered_a{0};
  std::atomic<int> delivered_b{0};
  if (!Check(
          bus.Subscribe("infer_a", entry.channel, [&](const mould::comm::MessageEnvelope&) {
            delivered_a.fetch_add(1, std::memory_order_relaxed);
          }),
          "subscribe infer_a")) {
    return false;
  }
  if (!Check(
          bus.Subscribe("infer_b", entry.channel, [&](const mould::comm::MessageEnvelope&) {
            delivered_b.fetch_add(1, std::memory_order_relaxed);
          }),
          "subscribe infer_b")) {
    return false;
  }
  if (!Check(bus.StartUnifiedSubscriberPump(), "pump")) {
    return false;
  }
  constexpr int kMessages = 24;
  for (int i = 0; i < kMessages; ++i) {
    std::vector<std::uint8_t> payload{
        static_cast<std::uint8_t>(i),
        static_cast<std::uint8_t>(i + 1),
    };
    if (!Check(bus.Publish("broker", entry.channel, payload), "publish")) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    const int sum =
        delivered_a.load(std::memory_order_acquire) + delivered_b.load(std::memory_order_acquire);
    if (sum >= kMessages) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const int da = delivered_a.load(std::memory_order_acquire);
  const int db = delivered_b.load(std::memory_order_acquire);
  if (!Check(da + db == kMessages, "compete: sum of deliveries equals publishes")) {
    return false;
  }
  control_plane.FinalizeUnlinkManagedSegments();
  return true;
}

bool TestBroadcastDeliveryMode_TwoConsumersStillFanOut() {
  mould::comm::MiddlewareConfig config;
  config.slot_payload_bytes = 64;
  config.backlog_alarm_threshold = config.slot_payload_bytes;
  config.default_consumer_slots_per_channel = 2;
  mould::comm::ShmBusControlPlane control_plane;
  const std::string channel =
      "split_ut_bc_" + std::to_string(getpid()) + "_" +
      std::to_string(
          static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));

  mould::config::ChannelTopologyIndex topology;
  mould::config::ChannelTopologyEntry entry;
  entry.channel = channel;
  entry.producers = {"broker"};
  entry.consumers = {"infer_a", "infer_b"};
  entry.consumer_count = 2;
  entry.params["delivery_mode"] = "broadcast";
  topology.emplace(entry.channel, entry);

  const std::string posix_name =
      mould::comm::BuildDeterministicShmName(mould::config::CanonicalShmChannelKey(entry));
  (void)shm_unlink(posix_name.c_str());

  if (!Check(control_plane.ProvisionChannelTopology(topology, config), "provision broadcast channel")) {
    return false;
  }
  mould::comm::ShmBusRuntime bus(config);
  if (!Check(bus.SetChannelTopology(topology), "runtime attach broadcast")) {
    return false;
  }
  if (!Check(bus.RegisterPublisher("broker", entry.channel), "publisher")) {
    return false;
  }
  std::atomic<int> delivered_a{0};
  std::atomic<int> delivered_b{0};
  if (!Check(
          bus.Subscribe("infer_a", entry.channel, [&](const mould::comm::MessageEnvelope&) {
            delivered_a.fetch_add(1, std::memory_order_relaxed);
          }),
          "subscribe infer_a")) {
    return false;
  }
  if (!Check(
          bus.Subscribe("infer_b", entry.channel, [&](const mould::comm::MessageEnvelope&) {
            delivered_b.fetch_add(1, std::memory_order_relaxed);
          }),
          "subscribe infer_b")) {
    return false;
  }
  if (!Check(bus.StartUnifiedSubscriberPump(), "pump")) {
    return false;
  }
  constexpr int kMessages = 6;
  for (int i = 0; i < kMessages; ++i) {
    std::vector<std::uint8_t> payload{static_cast<std::uint8_t>(i)};
    if (!Check(bus.Publish("broker", entry.channel, payload), "publish")) {
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
  const int da = delivered_a.load(std::memory_order_acquire);
  const int db = delivered_b.load(std::memory_order_acquire);
  if (!Check(da == kMessages && db == kMessages, "broadcast: full fan-out")) {
    return false;
  }
  control_plane.FinalizeUnlinkManagedSegments();
  return true;
}

bool TestTopologyResolve_DeliveryModeParams() {
  mould::config::ChannelTopologyEntry entry;
  entry.params["delivery_mode"] = "compete";
  if (!Check(
          mould::config::ResolveShmBusDeliveryModeForChannel(&entry) ==
              mould::config::ShmBusDeliveryMode::kCompete,
          "compete resolve")) {
    return false;
  }
  entry.params["delivery_mode"] = "broadcast";
  return Check(
      mould::config::ResolveShmBusDeliveryModeForChannel(&entry) ==
          mould::config::ShmBusDeliveryMode::kBroadcast,
      "broadcast resolve");
}

bool TestCompeteMode_StressSameProcessHeavyPublishThroughBus() {
  mould::comm::MiddlewareConfig config;
  config.slot_payload_bytes = 64;
  config.backlog_alarm_threshold = config.slot_payload_bytes;
  config.default_consumer_slots_per_channel = 2;
  mould::comm::ShmBusControlPlane control_plane;

  const std::string channel =
      "split_ut_compstress_sp_" + std::to_string(getpid()) + "_" +
      std::to_string(static_cast<std::uint64_t>(
          std::chrono::steady_clock::now().time_since_epoch().count()));

  mould::config::ChannelTopologyIndex topology;
  mould::config::ChannelTopologyEntry entry;
  entry.channel = channel;
  entry.producers = {"broker"};
  entry.consumers = {"infer_a", "infer_b"};
  entry.consumer_count = 2;
  entry.params["delivery_mode"] = "compete";
  topology.emplace(entry.channel, entry);

  const std::string posix =
      mould::comm::BuildDeterministicShmName(mould::config::CanonicalShmChannelKey(entry));
  (void)shm_unlink(posix.c_str());

  if (!Check(control_plane.ProvisionChannelTopology(topology, config), "stress sp compete provision")) {
    return false;
  }
  mould::comm::ShmBusRuntime bus(config);
  if (!Check(bus.SetChannelTopology(topology), "stress sp attach")) {
    control_plane.FinalizeUnlinkManagedSegments();
    return false;
  }
  if (!Check(bus.RegisterPublisher("broker", entry.channel), "stress sp publisher")) {
    control_plane.FinalizeUnlinkManagedSegments();
    return false;
  }
  std::atomic<int> delivered_a{0};
  std::atomic<int> delivered_b{0};
  if (!Check(
          bus.Subscribe(
              "infer_a",
              entry.channel,
              [&](const mould::comm::MessageEnvelope&) {
                delivered_a.fetch_add(1, std::memory_order_relaxed);
              }),
          "stress sp subscribe a")) {
    control_plane.FinalizeUnlinkManagedSegments();
    return false;
  }
  if (!Check(
          bus.Subscribe(
              "infer_b",
              entry.channel,
              [&](const mould::comm::MessageEnvelope&) {
                delivered_b.fetch_add(1, std::memory_order_relaxed);
              }),
          "stress sp subscribe b")) {
    control_plane.FinalizeUnlinkManagedSegments();
    return false;
  }
  if (!Check(bus.StartUnifiedSubscriberPump(), "stress sp pump")) {
    control_plane.FinalizeUnlinkManagedSegments();
    return false;
  }

  constexpr int kStress = 15000;
  for (int i = 0; i < kStress; ++i) {
    std::vector<std::uint8_t> payload{
        static_cast<std::uint8_t>(i & 0xFF),
        static_cast<std::uint8_t>((static_cast<unsigned>(i) >> 8U) & 0xFF),
    };
    if (!Check(bus.Publish("broker", entry.channel, payload), "stress sp publish burst")) {
      control_plane.FinalizeUnlinkManagedSegments();
      return false;
    }
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
  while (std::chrono::steady_clock::now() < deadline) {
    const int sum =
        delivered_a.load(std::memory_order_acquire) + delivered_b.load(std::memory_order_acquire);
    if (sum >= kStress) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const int da = delivered_a.load(std::memory_order_acquire);
  const int db = delivered_b.load(std::memory_order_acquire);
  if (!Check(da + db == kStress, "stress sp: deliveries == publishes")) {
    control_plane.FinalizeUnlinkManagedSegments();
    return false;
  }
  control_plane.FinalizeUnlinkManagedSegments();
  return true;
}

bool TestCompeteMode_MultiprocessStressTwoConsumersInheritedFds() {
  mould::comm::MiddlewareConfig cfg;
  cfg.slot_payload_bytes = 64;
  cfg.backlog_alarm_threshold = cfg.slot_payload_bytes;
  cfg.default_consumer_slots_per_channel = 2;
  mould::comm::ShmBusControlPlane control_plane;
  // Many messages × multi-process pumps: intentionally remove producer pacing and increase count
  // so the ring is repeatedly driven to full occupancy under compete mode.
  constexpr std::uint32_t kStressPublishes = 20000U;

  const std::string channel =
      "split_ut_compstress_mp_" + std::to_string(getpid()) + "_" +
      std::to_string(static_cast<std::uint64_t>(
          std::chrono::steady_clock::now().time_since_epoch().count()));

  mould::config::ChannelTopologyIndex topology;
  mould::config::ChannelTopologyEntry entry;
  entry.channel = channel;
  entry.producers = {"broker"};
  entry.consumers = {"infer_a", "infer_b"};
  entry.consumer_count = 2;
  entry.params["delivery_mode"] = "compete";
  // Keep ring moderate to make "full ring + reclaim + compete" happen frequently.
  entry.params["shm_slot_count"] = "128";
  topology.emplace(channel, entry);

  const std::string posix =
      mould::comm::BuildDeterministicShmName(mould::config::CanonicalShmChannelKey(entry));
  (void)shm_unlink(posix.c_str());

  if (!Check(control_plane.ProvisionChannelTopology(topology, cfg), "mp compete stress provision")) {
    return false;
  }

  long psz = sysconf(_SC_PAGESIZE);
  if (psz < static_cast<long>(sizeof(std::uint32_t))) {
    psz = static_cast<long>(sizeof(std::uint32_t));
  }
  void* shared =
      mmap(nullptr, static_cast<std::size_t>(psz), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (!Check(shared != MAP_FAILED, "mp stress mmap anon")) {
    control_plane.FinalizeUnlinkManagedSegments();
    return false;
  }
  auto* deli = reinterpret_cast<std::uint32_t*>(shared);
  std::atomic_ref<std::uint32_t>(*deli).store(0, std::memory_order_relaxed);

  int ready_a[2] = {-1, -1};
  int ready_b[2] = {-1, -1};
  if (!Check(pipe(ready_a) == 0 && pipe(ready_b) == 0, "ready pipes")) {
    munmap(shared, static_cast<std::size_t>(psz));
    control_plane.FinalizeUnlinkManagedSegments();
    return false;
  }
  (void)fcntl(ready_a[1], F_SETFD, FD_CLOEXEC);
  (void)fcntl(ready_b[1], F_SETFD, FD_CLOEXEC);

  const pid_t pid_a = fork();
  if (!Check(pid_a >= 0, "fork consume a")) {
    close(ready_a[0]);
    close(ready_a[1]);
    close(ready_b[0]);
    close(ready_b[1]);
    munmap(shared, static_cast<std::size_t>(psz));
    control_plane.FinalizeUnlinkManagedSegments();
    return false;
  }

  if (pid_a == 0) {
    close(ready_a[0]);
    close(ready_b[0]);
    close(ready_b[1]);
    mould::comm::ShmBusRuntime bus(cfg);
    const auto handshake_fail = [&]() {
      const std::uint8_t v = 0;
      (void)WriteFully(ready_a[1], &v, sizeof(v));
    };
    if (!bus.SetChannelTopology(topology)) {
      handshake_fail();
      close(ready_a[1]);
      _exit(41);
    }
    if (!bus.Subscribe(
            "infer_a",
            channel,
            [deli](const mould::comm::MessageEnvelope&) {
              std::atomic_ref<std::uint32_t>(*deli).fetch_add(1, std::memory_order_relaxed);
            })) {
      handshake_fail();
      close(ready_a[1]);
      _exit(42);
    }
    if (!bus.StartUnifiedSubscriberPump()) {
      handshake_fail();
      close(ready_a[1]);
      _exit(43);
    }
    const std::uint8_t ok = 1;
    if (!WriteFully(ready_a[1], &ok, sizeof(ok))) {
      handshake_fail();
      close(ready_a[1]);
      _exit(44);
    }
    close(ready_a[1]);
    for (;;) {
      sleep(3600);
    }
  }

  close(ready_a[1]);
  std::uint8_t ra = 0;
  if (!Check(ReadFully(ready_a[0], &ra, sizeof(ra)) && ra == 1, "handshake consumer a")) {
    (void)kill(pid_a, SIGKILL);
    (void)waitpid(pid_a, nullptr, 0);
    close(ready_a[0]);
    close(ready_b[0]);
    close(ready_b[1]);
    munmap(shared, static_cast<std::size_t>(psz));
    control_plane.FinalizeUnlinkManagedSegments();
    return false;
  }

  close(ready_a[0]);

  const pid_t pid_b = fork();
  if (!Check(pid_b >= 0, "fork consume b")) {
    (void)kill(pid_a, SIGKILL);
    (void)waitpid(pid_a, nullptr, 0);
    close(ready_b[0]);
    close(ready_b[1]);
    munmap(shared, static_cast<std::size_t>(psz));
    control_plane.FinalizeUnlinkManagedSegments();
    return false;
  }

  if (pid_b == 0) {
    close(ready_a[0]);
    close(ready_a[1]);
    close(ready_b[0]);
    mould::comm::ShmBusRuntime bus(cfg);
    const auto handshake_fail = [&]() {
      const std::uint8_t v = 0;
      (void)WriteFully(ready_b[1], &v, sizeof(v));
    };
    if (!bus.SetChannelTopology(topology)) {
      handshake_fail();
      close(ready_b[1]);
      _exit(51);
    }
    if (!bus.Subscribe(
            "infer_b",
            channel,
            [deli](const mould::comm::MessageEnvelope&) {
              std::atomic_ref<std::uint32_t>(*deli).fetch_add(1, std::memory_order_relaxed);
            })) {
      handshake_fail();
      close(ready_b[1]);
      _exit(52);
    }
    if (!bus.StartUnifiedSubscriberPump()) {
      handshake_fail();
      close(ready_b[1]);
      _exit(53);
    }
    const std::uint8_t ok = 1;
    if (!WriteFully(ready_b[1], &ok, sizeof(ok))) {
      handshake_fail();
      close(ready_b[1]);
      _exit(54);
    }
    close(ready_b[1]);
    for (;;) {
      sleep(3600);
    }
  }

  close(ready_b[1]);
  std::uint8_t rb = 0;
  if (!Check(ReadFully(ready_b[0], &rb, sizeof(rb)) && rb == 1, "handshake consumer b")) {
    (void)kill(pid_a, SIGKILL);
    (void)kill(pid_b, SIGKILL);
    (void)waitpid(pid_a, nullptr, 0);
    (void)waitpid(pid_b, nullptr, 0);
    close(ready_b[0]);
    munmap(shared, static_cast<std::size_t>(psz));
    control_plane.FinalizeUnlinkManagedSegments();
    return false;
  }
  close(ready_b[0]);

  const pid_t producer = fork();
  if (!Check(producer >= 0, "fork producer stress")) {
    (void)kill(pid_a, SIGKILL);
    (void)kill(pid_b, SIGKILL);
    (void)waitpid(pid_a, nullptr, 0);
    (void)waitpid(pid_b, nullptr, 0);
    munmap(shared, static_cast<std::size_t>(psz));
    control_plane.FinalizeUnlinkManagedSegments();
    return false;
  }

  if (producer == 0) {
    mould::comm::ShmBusRuntime bus(cfg);
    if (!bus.SetChannelTopology(topology)) {
      _exit(61);
    }
    if (!bus.RegisterPublisher("broker", channel)) {
      _exit(62);
    }
    for (std::uint32_t i = 0; i < kStressPublishes; ++i) {
      bool published = false;
      while (!published) {
        std::vector<std::uint8_t> payload{0xC0, static_cast<std::uint8_t>(i & 0xFF)};
        published = bus.Publish("broker", channel, payload);
        ::usleep(100);
        if (!published) {
          ::usleep(100);
          std::this_thread::yield();
        }
      }
    }
    _exit(0);
  }

  int pst = 0;
  if (!Check(waitpid(producer, &pst, 0) == producer && WIFEXITED(pst) && WEXITSTATUS(pst) == 0, "producer")) {
    (void)kill(pid_a, SIGKILL);
    (void)kill(pid_b, SIGKILL);
    (void)waitpid(pid_a, nullptr, 0);
    (void)waitpid(pid_b, nullptr, 0);
    munmap(shared, static_cast<std::size_t>(psz));
    control_plane.FinalizeUnlinkManagedSegments();
    return false;
  }

  if (!Check(
          WaitAtomicRefUintAtLeast(deli, kStressPublishes, 18000),
          "mp stress combined deliveries")) {
    (void)kill(pid_a, SIGKILL);
    (void)kill(pid_b, SIGKILL);
    (void)waitpid(pid_a, nullptr, 0);
    (void)waitpid(pid_b, nullptr, 0);
    munmap(shared, static_cast<std::size_t>(psz));
    control_plane.FinalizeUnlinkManagedSegments();
    return false;
  }

  (void)kill(pid_a, SIGTERM);
  (void)kill(pid_b, SIGTERM);
  (void)waitpid(pid_a, nullptr, 0);
  (void)waitpid(pid_b, nullptr, 0);
  munmap(shared, static_cast<std::size_t>(psz));
  control_plane.FinalizeUnlinkManagedSegments();
  return true;
}

bool TestConsumerCrash_OfflineAndReclaim() {
  constexpr std::uint32_t kSlots = 8;
  constexpr std::uint32_t kConsumers = 2;
  constexpr std::uint64_t kPayload = 256;
  const std::size_t layout_bytes = mould::comm::ComputeRingLayoutSizeBytes(kSlots, kConsumers);
  std::vector<std::uint8_t> mem(layout_bytes + kPayload, 0);
  auto ring = mould::comm::RingLayoutView::Initialize(
      mem.data(), mem.size(), kSlots, kConsumers, kPayload);
  if (!Check(ring.has_value(), "ring init")) {
    return false;
  }
  if (!Check(ring->AddConsumerOnline(0), "consumer 0 online")) {
    return false;
  }
  const auto* consumers = ring->Consumers();
  if (!Check(consumers != nullptr, "consumers")) {
    return false;
  }
  const std::uint32_t pid = consumers[0].owner_pid;
  const std::uint32_t epoch = consumers[0].owner_start_epoch;
  const std::uint64_t offline_before =
      ring->Header()->consumer_offline_events.load(std::memory_order_relaxed);
  if (!Check(ring->TakeConsumerOfflineIfOwner(0, pid, epoch), "supervisor offline")) {
    return false;
  }
  const std::uint64_t offline_after =
      ring->Header()->consumer_offline_events.load(std::memory_order_relaxed);
  if (!Check(offline_after > offline_before, "offline should bump consumer_offline_events")) {
    return false;
  }
  (void)ring->ReclaimCommittedSlots();
  return Check(
      consumers[0].state.load(std::memory_order_acquire) ==
          static_cast<std::uint32_t>(mould::comm::ConsumerState::kOffline),
      "slot should stay offline");
}

bool TestPidReuse_OwnerEpochDisambiguates() {
  constexpr std::uint32_t kSlots = 4;
  constexpr std::uint32_t kConsumers = 2;
  constexpr std::uint64_t kPayload = 128;
  const std::size_t layout_bytes = mould::comm::ComputeRingLayoutSizeBytes(kSlots, kConsumers);
  std::vector<std::uint8_t> mem(layout_bytes + kPayload, 0);
  auto ring = mould::comm::RingLayoutView::Initialize(
      mem.data(), mem.size(), kSlots, kConsumers, kPayload);
  if (!Check(ring.has_value(), "ring init")) {
    return false;
  }
  if (!Check(ring->AddConsumerOnline(0), "online")) {
    return false;
  }
  auto* consumers = ring->Consumers();
  const std::uint32_t pid = consumers[0].owner_pid;
  const std::uint32_t epoch = consumers[0].owner_start_epoch;
  if (!Check(!ring->TakeConsumerOfflineIfOwner(0, pid, static_cast<std::uint32_t>(epoch + 999U)), "wrong epoch")) {
    return false;
  }
  return Check(ring->TakeConsumerOfflineIfOwner(0, pid, epoch), "correct tuple");
}

bool TestPreforkSharedControlPlaneCowAddressStableAfterFork() {
  ShmBusRuntimeFinalizeSharedControlPlaneShm();
  ShmBusRuntimePreforkEnsureSharedControlPlane();
  const void* parent_ptr = ShmBusRuntimeGetSharedControlPlane().get();
  const pid_t child = fork();
  if (!Check(child >= 0, "fork")) {
    return false;
  }
  if (child == 0) {
    const void* child_ptr = ShmBusRuntimeGetSharedControlPlane().get();
    _exit(child_ptr == parent_ptr ? 0 : 31);
  }
  int st = 0;
  if (!Check(waitpid(child, &st, 0) == child && WIFEXITED(st) && WEXITSTATUS(st) == 0, "child sees same plane")) {
    return false;
  }
  ShmBusRuntimeFinalizeSharedControlPlaneShm();
  return true;
}

bool TestSupervisorTakeConsumerOfflineOnLiveBus() {
  mould::comm::MiddlewareConfig cfg;
  mould::comm::ShmBusControlPlane control_plane;
  mould::comm::ShmBusRuntime bus(cfg);
  const std::string channel =
      "split_ut_sup_" + std::to_string(getpid()) + "_" +
      std::to_string(
          static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));

  mould::config::ChannelTopologyIndex topology;
  mould::config::ChannelTopologyEntry entry;
  entry.channel = channel;
  entry.producers = {"broker"};
  entry.consumers = {"infer"};
  entry.consumer_count = 1;
  topology.emplace(channel, entry);

  const std::string posix = mould::comm::BuildDeterministicShmName(
      mould::config::CanonicalShmChannelKey(entry));
  (void)shm_unlink(posix.c_str());

  if (!Check(control_plane.ProvisionChannelTopology(topology, cfg), "topology")) {
    return false;
  }
  if (!Check(bus.SetChannelTopology(topology), "runtime attach")) {
    return false;
  }
  if (!Check(bus.RegisterPublisher("broker", channel), "publisher")) {
    return false;
  }
  if (!Check(
          bus.Subscribe("infer", channel, [](const mould::comm::MessageEnvelope&) {}),
          "subscribe")) {
    return false;
  }
  std::uint32_t pid = 0;
  std::uint32_t epoch = 0;
  const std::size_t slot_payload_bytes = cfg.slot_payload_bytes;
  if (!Check(
          control_plane.ReadConsumerOwnerIdentity(channel, &entry, slot_payload_bytes, 0, &pid, &epoch),
          "read owner tuple")) {
    return false;
  }
  if (!Check(
          !control_plane.TakeConsumerOfflineIfOwner(
              channel, &entry, slot_payload_bytes, 0, pid, static_cast<std::uint32_t>(epoch + 999U)),
          "wrong epoch should not offline")) {
    return false;
  }
  if (!Check(
          control_plane.TakeConsumerOfflineIfOwner(channel, &entry, slot_payload_bytes, 0, pid, epoch),
          "supervisor offline")) {
    return false;
  }
  if (!Check(
          !control_plane.TakeConsumerOfflineIfOwner(channel, &entry, slot_payload_bytes, 0, pid, epoch),
          "second offline should fail")) {
    return false;
  }
  control_plane.FinalizeUnlinkManagedSegments();
  return true;
}

/// 目的：验证 `ShmBusControlPlane::TakeAllConsumersOfflineForProcessPid` 能按 PID 扫掉拓扑里
/// 所有在线槽位；第二次扫描应为 0；下线后 `ReadConsumerOwnerIdentity` 失败。
bool TestControlPlane_TakeAllConsumersOfflineForProcessPid_one_channel() {
  mould::comm::MiddlewareConfig cfg;
  mould::comm::ShmBusControlPlane control_plane;
  mould::comm::ShmBusRuntime bus(cfg);
  const std::string channel =
      "split_ut_all1_" + std::to_string(getpid()) + "_" +
      std::to_string(
          static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));

  mould::config::ChannelTopologyIndex topology;
  mould::config::ChannelTopologyEntry entry;
  entry.channel = channel;
  entry.producers = {"broker"};
  entry.consumers = {"infer"};
  entry.consumer_count = 1;
  topology.emplace(channel, entry);

  const std::string posix = mould::comm::BuildDeterministicShmName(
      mould::config::CanonicalShmChannelKey(entry));
  (void)shm_unlink(posix.c_str());

  if (!Check(control_plane.ProvisionChannelTopology(topology, cfg), "provision for take-all")) {
    return false;
  }
  if (!Check(bus.SetChannelTopology(topology), "runtime attach")) {
    return false;
  }
  if (!Check(
          bus.Subscribe("infer", channel, [](const mould::comm::MessageEnvelope&) {}),
          "subscribe")) {
    return false;
  }
  const std::size_t slot_payload_bytes = cfg.slot_payload_bytes;
  const std::uint32_t self_pid = static_cast<std::uint32_t>(getpid());
  const std::uint32_t swept =
      control_plane.TakeAllConsumersOfflineForProcessPid(self_pid, topology, slot_payload_bytes);
  if (!Check(swept == 1, "take-all should offline exactly one slot")) {
    return false;
  }
  const std::uint32_t swept_again =
      control_plane.TakeAllConsumersOfflineForProcessPid(self_pid, topology, slot_payload_bytes);
  if (!Check(swept_again == 0, "second take-all should be idempotent")) {
    return false;
  }
  std::uint32_t pid = 0;
  std::uint32_t epoch = 0;
  if (!Check(
          !control_plane.ReadConsumerOwnerIdentity(channel, &entry, slot_payload_bytes, 0, &pid, &epoch),
          "consumer should no longer be online")) {
    return false;
  }
  control_plane.FinalizeUnlinkManagedSegments();
  return true;
}

/// 目的：同一进程订阅两个逻辑 channel 时，`TakeAllConsumersOfflineForProcessPid` 应对两个段各下线
/// 一个槽位，返回计数 2；错误 PID 时返回 0 且不改变在线状态。
bool TestControlPlane_TakeAllConsumersOfflineForProcessPid_two_channels() {
  mould::comm::MiddlewareConfig cfg;
  mould::comm::ShmBusControlPlane control_plane;
  mould::comm::ShmBusRuntime bus(cfg);
  const std::string suffix = std::to_string(getpid()) + "_" +
      std::to_string(
          static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
  const std::string channel_a = "split_ut_all2_a_" + suffix;
  const std::string channel_b = "split_ut_all2_b_" + suffix;

  mould::config::ChannelTopologyIndex topology;
  mould::config::ChannelTopologyEntry entry_a;
  entry_a.channel = channel_a;
  entry_a.producers = {"broker"};
  entry_a.consumers = {"infer"};
  entry_a.consumer_count = 1;
  mould::config::ChannelTopologyEntry entry_b = entry_a;
  entry_b.channel = channel_b;
  topology.emplace(channel_a, entry_a);
  topology.emplace(channel_b, entry_b);

  (void)shm_unlink(mould::comm::BuildDeterministicShmName(mould::config::CanonicalShmChannelKey(entry_a)).c_str());
  (void)shm_unlink(mould::comm::BuildDeterministicShmName(mould::config::CanonicalShmChannelKey(entry_b)).c_str());

  if (!Check(control_plane.ProvisionChannelTopology(topology, cfg), "provision two channels")) {
    return false;
  }
  if (!Check(bus.SetChannelTopology(topology), "runtime attach")) {
    return false;
  }
  const auto noop = [](const mould::comm::MessageEnvelope&) {};
  if (!Check(bus.Subscribe("infer", channel_a, noop), "subscribe a")) {
    return false;
  }
  if (!Check(bus.Subscribe("infer", channel_b, noop), "subscribe b")) {
    return false;
  }
  const std::size_t slot_payload_bytes = cfg.slot_payload_bytes;
  const std::uint32_t self_pid = static_cast<std::uint32_t>(getpid());
  const std::uint32_t wrong_pid = self_pid + 404040U;
  if (!Check(
          control_plane.TakeAllConsumersOfflineForProcessPid(wrong_pid, topology, slot_payload_bytes) == 0,
          "wrong pid should not offline any slot")) {
    return false;
  }
  std::uint32_t pid_a = 0;
  std::uint32_t epoch_a = 0;
  if (!Check(
          control_plane.ReadConsumerOwnerIdentity(channel_a, &entry_a, slot_payload_bytes, 0, &pid_a, &epoch_a),
          "channel a still online after wrong-pid sweep")) {
    return false;
  }
  const std::uint32_t swept =
      control_plane.TakeAllConsumersOfflineForProcessPid(self_pid, topology, slot_payload_bytes);
  if (!Check(swept == 2, "take-all should offline one slot per channel")) {
    return false;
  }
  if (!Check(
          !control_plane.ReadConsumerOwnerIdentity(channel_a, &entry_a, slot_payload_bytes, 0, &pid_a, &epoch_a),
          "channel a offline")) {
    return false;
  }
  std::uint32_t pid_b = 0;
  std::uint32_t epoch_b = 0;
  if (!Check(
          !control_plane.ReadConsumerOwnerIdentity(channel_b, &entry_b, slot_payload_bytes, 0, &pid_b, &epoch_b),
          "channel b offline")) {
    return false;
  }
  control_plane.FinalizeUnlinkManagedSegments();
  return true;
}

constexpr std::uint32_t kSplitRolesWarmupPublishCount = 5;

/// 父进程仅 `ShmBusControlPlane`；子进程仅 `ShmBusRuntime`。
/// 必须在父进程 `ProvisionChannelTopology`（内部 `Initialize` ring 并 `eventfd`）之后立刻 `fork`，
/// 这样子进程继承与 SHM notification 表一致的 fd 号，才能在本进程新建 `ShmBusRuntime` 并 `Subscribe`。
///
/// 场景一：生产者与消费者**分属两个子进程**且同时在线。父进程仍只跑控制面；两个子进程均在父进程
/// `ProvisionChannelTopology` 之后从父进程 `fork`，共享继承的 fd 表，因而各自 `ShmBusRuntime` attach 后
/// notification fd 与 SHM 一致。必须先让消费者子进程 `Subscribe` 并向父进程发就绪信号，再 `fork` 生产者
/// 子进程发布预热条数（若先发布后订阅，新消费者不会回放已提交消息，且无在线消费者时槽位易被占满）。
bool TestSplitControlParent_DataChild_BothPublisherAndConsumerOnline() {
  mould::comm::MiddlewareConfig cfg;
  mould::comm::ShmBusControlPlane control_plane;
  const std::string channel =
      "split_roles_on_" + std::to_string(getpid()) + "_" +
      std::to_string(
          static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
  mould::config::ChannelTopologyIndex topology;
  mould::config::ChannelTopologyEntry entry;
  entry.channel = channel;
  entry.producers = {"broker"};
  entry.consumers = {"infer"};
  entry.consumer_count = 1;
  topology.emplace(channel, entry);
  (void)shm_unlink(mould::comm::BuildDeterministicShmName(mould::config::CanonicalShmChannelKey(entry)).c_str());
  if (!Check(control_plane.ProvisionChannelTopology(topology, cfg), "provision")) {
    return false;
  }

  int ready_pipe[2] = {-1, -1};
  int result_pipe[2] = {-1, -1};
  if (!Check(pipe(ready_pipe) == 0 && pipe(result_pipe) == 0, "pipes")) {
    if (ready_pipe[0] >= 0) {
      close(ready_pipe[0]);
    }
    if (ready_pipe[1] >= 0) {
      close(ready_pipe[1]);
    }
    if (result_pipe[0] >= 0) {
      close(result_pipe[0]);
    }
    if (result_pipe[1] >= 0) {
      close(result_pipe[1]);
    }
    return false;
  }
  (void)fcntl(ready_pipe[1], F_SETFD, FD_CLOEXEC);
  (void)fcntl(result_pipe[1], F_SETFD, FD_CLOEXEC);

  const pid_t consumer = fork();
  if (!Check(consumer >= 0, "fork consumer")) {
    close(ready_pipe[0]);
    close(ready_pipe[1]);
    close(result_pipe[0]);
    close(result_pipe[1]);
    return false;
  }
  if (consumer == 0) {
    close(ready_pipe[0]);
    close(result_pipe[0]);
    std::atomic<std::uint32_t> delivered{0};
    const auto handshake_fail = [&]() {
      const std::uint8_t fail = 0;
      (void)WriteFully(ready_pipe[1], &fail, sizeof(fail));
    };
    {
      mould::comm::ShmBusRuntime bus(cfg);
      if (!bus.SetChannelTopology(topology)) {
        handshake_fail();
        close(ready_pipe[1]);
        close(result_pipe[1]);
        _exit(21);
      }
      if (!bus.Subscribe(
              "infer",
              channel,
              [&delivered](const mould::comm::MessageEnvelope&) {
                delivered.fetch_add(1, std::memory_order_relaxed);
              })) {
        handshake_fail();
        close(ready_pipe[1]);
        close(result_pipe[1]);
        _exit(22);
      }
      if (!bus.StartUnifiedSubscriberPump()) {
        handshake_fail();
        close(ready_pipe[1]);
        close(result_pipe[1]);
        _exit(22);
      }
      const std::uint8_t ready = 1;
      if (!WriteFully(ready_pipe[1], &ready, sizeof(ready))) {
        handshake_fail();
        close(ready_pipe[1]);
        close(result_pipe[1]);
        _exit(23);
      }
      if (!WaitAtomicUintAtLeast(&delivered, kSplitRolesWarmupPublishCount)) {
        const std::uint32_t z = delivered.load(std::memory_order_relaxed);
        (void)WriteFully(result_pipe[1], &z, sizeof(z));
        close(ready_pipe[1]);
        close(result_pipe[1]);
        _exit(24);
      }
      const std::uint32_t done = delivered.load(std::memory_order_relaxed);
      (void)WriteFully(result_pipe[1], &done, sizeof(done));
    }
    close(ready_pipe[1]);
    close(result_pipe[1]);
    _exit(0);
  }

  close(ready_pipe[1]);
  close(result_pipe[1]);
  std::uint8_t ready_byte = 0;
  if (!Check(ReadFully(ready_pipe[0], &ready_byte, sizeof(ready_byte)) && ready_byte == 1, "consumer subscribed")) {
    close(ready_pipe[0]);
    close(result_pipe[0]);
    (void)waitpid(consumer, nullptr, 0);
    control_plane.FinalizeUnlinkManagedSegments();
    return false;
  }
  close(ready_pipe[0]);

  const pid_t producer = fork();
  if (!Check(producer >= 0, "fork producer")) {
    close(result_pipe[0]);
    (void)kill(consumer, SIGTERM);
    (void)waitpid(consumer, nullptr, 0);
    control_plane.FinalizeUnlinkManagedSegments();
    return false;
  }
  if (producer == 0) {
    close(result_pipe[0]);
    close(result_pipe[1]);
    mould::comm::ShmBusRuntime bus(cfg);
    if (!bus.SetChannelTopology(topology)) {
      _exit(31);
    }
    if (!bus.RegisterPublisher("broker", channel)) {
      _exit(32);
    }
    for (std::uint32_t i = 0; i < kSplitRolesWarmupPublishCount; ++i) {
      const std::vector<std::uint8_t> payload{0x11, static_cast<std::uint8_t>(i)};
      if (!bus.Publish("broker", channel, payload)) {
        _exit(33);
      }
    }
    _exit(0);
  }

  int pst = 0;
  if (!Check(waitpid(producer, &pst, 0) == producer, "waitpid producer")) {
    close(result_pipe[0]);
    (void)kill(consumer, SIGTERM);
    (void)waitpid(consumer, nullptr, 0);
    control_plane.FinalizeUnlinkManagedSegments();
    return false;
  }
  if (!Check(WIFEXITED(pst) && WEXITSTATUS(pst) == 0, "producer exit ok")) {
    close(result_pipe[0]);
    (void)kill(consumer, SIGTERM);
    (void)waitpid(consumer, nullptr, 0);
    control_plane.FinalizeUnlinkManagedSegments();
    return false;
  }

  std::uint32_t reported = 0;
  const bool read_ok = ReadFully(result_pipe[0], &reported, sizeof(reported));
  close(result_pipe[0]);
  int cst = 0;
  if (!Check(waitpid(consumer, &cst, 0) == consumer, "waitpid consumer")) {
    control_plane.FinalizeUnlinkManagedSegments();
    return false;
  }
  control_plane.FinalizeUnlinkManagedSegments();
  return Check(
      read_ok && WIFEXITED(cst) && WEXITSTATUS(cst) == 0 &&
          reported == kSplitRolesWarmupPublishCount,
      "consumer child should receive all producer child warmup publishes");
}

/// 场景二：仅生产者子进程发布若干条后退出；消费者留在子进程（孙进程为生产者）。
/// 预期：消费者仍能收齐孙进程在退出前写入的全部消息。
bool TestSplitControlParent_DataChild_ProducerChildExitsConsumerStillLive() {
  mould::comm::MiddlewareConfig cfg;
  mould::comm::ShmBusControlPlane control_plane;
  const std::string channel =
      "split_roles_pe_" + std::to_string(getpid()) + "_" +
      std::to_string(
          static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
  mould::config::ChannelTopologyIndex topology;
  mould::config::ChannelTopologyEntry entry;
  entry.channel = channel;
  entry.producers = {"broker"};
  entry.consumers = {"infer"};
  entry.consumer_count = 1;
  topology.emplace(channel, entry);
  (void)shm_unlink(mould::comm::BuildDeterministicShmName(mould::config::CanonicalShmChannelKey(entry)).c_str());
  if (!Check(control_plane.ProvisionChannelTopology(topology, cfg), "provision")) {
    return false;
  }

  int p2c[2] = {-1, -1};
  if (!Check(pipe(p2c) == 0, "pipe")) {
    return false;
  }
  (void)fcntl(p2c[1], F_SETFD, FD_CLOEXEC);
  const pid_t worker = fork();
  if (!Check(worker >= 0, "fork worker")) {
    close(p2c[0]);
    close(p2c[1]);
    return false;
  }
  if (worker == 0) {
    close(p2c[0]);
    std::atomic<std::uint32_t> delivered{0};
    {
      mould::comm::ShmBusRuntime bus(cfg);
      if (!bus.SetChannelTopology(topology)) {
        const std::uint32_t z = delivered.load(std::memory_order_relaxed);
        (void)WriteFully(p2c[1], &z, sizeof(z));
        close(p2c[1]);
        _exit(31);
      }
      if (!bus.Subscribe(
              "infer",
              channel,
              [&delivered](const mould::comm::MessageEnvelope&) {
                delivered.fetch_add(1, std::memory_order_relaxed);
              })) {
        const std::uint32_t z = delivered.load(std::memory_order_relaxed);
        (void)WriteFully(p2c[1], &z, sizeof(z));
        close(p2c[1]);
        _exit(32);
      }
      if (!bus.StartUnifiedSubscriberPump()) {
        const std::uint32_t z = delivered.load(std::memory_order_relaxed);
        (void)WriteFully(p2c[1], &z, sizeof(z));
        close(p2c[1]);
        _exit(32);
      }
      const pid_t producer = fork();
      if (producer < 0) {
        const std::uint32_t z = delivered.load(std::memory_order_relaxed);
        (void)WriteFully(p2c[1], &z, sizeof(z));
        close(p2c[1]);
        _exit(33);
      }
      if (producer == 0) {
        close(p2c[1]);
        mould::comm::ShmBusRuntime pub_bus(cfg);
        if (!pub_bus.SetChannelTopology(topology)) {
          _exit(34);
        }
        if (!pub_bus.RegisterPublisher("broker", channel)) {
          _exit(35);
        }
        for (std::uint32_t i = 0; i < kSplitRolesWarmupPublishCount; ++i) {
          const std::vector<std::uint8_t> payload{0x22, static_cast<std::uint8_t>(i)};
          if (!pub_bus.Publish("broker", channel, payload)) {
            _exit(36);
          }
        }
        _exit(0);
      }
      int pst = 0;
      if (waitpid(producer, &pst, 0) != producer || !WIFEXITED(pst) || WEXITSTATUS(pst) != 0) {
        const std::uint32_t z = delivered.load(std::memory_order_relaxed);
        (void)WriteFully(p2c[1], &z, sizeof(z));
        close(p2c[1]);
        _exit(37);
      }
      if (!WaitAtomicUintAtLeast(&delivered, kSplitRolesWarmupPublishCount)) {
        const std::uint32_t z = delivered.load(std::memory_order_relaxed);
        (void)WriteFully(p2c[1], &z, sizeof(z));
        close(p2c[1]);
        _exit(38);
      }
      const std::uint32_t done = delivered.load(std::memory_order_relaxed);
      (void)WriteFully(p2c[1], &done, sizeof(done));
    }
    close(p2c[1]);
    _exit(0);
  }
  close(p2c[1]);
  std::uint32_t reported = 0;
  const bool read_ok = ReadFully(p2c[0], &reported, sizeof(reported));
  close(p2c[0]);
  int st = 0;
  if (!Check(waitpid(worker, &st, 0) == worker, "waitpid worker")) {
    control_plane.FinalizeUnlinkManagedSegments();
    return false;
  }
  control_plane.FinalizeUnlinkManagedSegments();
  return Check(
      read_ok && WIFEXITED(st) && WEXITSTATUS(st) == 0 &&
          reported == kSplitRolesWarmupPublishCount,
      "consumer should drain producer child publishes after producer exits");
}

/// 场景三：子进程生产者仍在线；父进程通过控制面把该子进程上的消费者槽下线。
/// 预期：`TakeAllConsumersOfflineForProcessPid` 返回 1；子进程在消费者下线后仍能 `Publish`（生产者在线），
/// 且不再收到新的投递（回调计数保持为已预热条数）。
bool TestSplitControlParent_DataChild_ProducerLiveConsumerTakenOfflineBySupervisor() {
  mould::comm::MiddlewareConfig cfg;
  mould::comm::ShmBusControlPlane control_plane;
  const std::string channel =
      "split_roles_cl_" + std::to_string(getpid()) + "_" +
      std::to_string(
          static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
  mould::config::ChannelTopologyIndex topology;
  mould::config::ChannelTopologyEntry entry;
  entry.channel = channel;
  entry.producers = {"broker"};
  entry.consumers = {"infer"};
  entry.consumer_count = 1;
  topology.emplace(channel, entry);
  const std::size_t slot_payload_bytes = cfg.slot_payload_bytes;
  (void)shm_unlink(mould::comm::BuildDeterministicShmName(mould::config::CanonicalShmChannelKey(entry)).c_str());
  if (!Check(control_plane.ProvisionChannelTopology(topology, cfg), "provision")) {
    return false;
  }

  int w2p[2] = {-1, -1};
  int p2w[2] = {-1, -1};
  if (!Check(pipe(w2p) == 0 && pipe(p2w) == 0, "pipes")) {
    if (w2p[0] >= 0) {
      close(w2p[0]);
    }
    if (w2p[1] >= 0) {
      close(w2p[1]);
    }
    if (p2w[0] >= 0) {
      close(p2w[0]);
    }
    if (p2w[1] >= 0) {
      close(p2w[1]);
    }
    return false;
  }
  (void)fcntl(w2p[1], F_SETFD, FD_CLOEXEC);
  (void)fcntl(p2w[0], F_SETFD, FD_CLOEXEC);
  const pid_t worker = fork();
  if (!Check(worker >= 0, "fork worker")) {
    close(w2p[0]);
    close(w2p[1]);
    close(p2w[0]);
    close(p2w[1]);
    return false;
  }
  if (worker == 0) {
    close(w2p[0]);
    close(p2w[1]);
    std::atomic<std::uint32_t> delivered{0};
    std::uint8_t extra_pub_ok = 0;
    {
      mould::comm::ShmBusRuntime bus(cfg);
      const auto fail_handshake = [&]() {
        const std::uint8_t fail = 0;
        (void)WriteFully(w2p[1], &fail, sizeof(fail));
      };
      if (!bus.SetChannelTopology(topology)) {
        fail_handshake();
        close(w2p[1]);
        close(p2w[0]);
        _exit(41);
      }
      if (!bus.RegisterPublisher("broker", channel)) {
        fail_handshake();
        close(w2p[1]);
        close(p2w[0]);
        _exit(42);
      }
      if (!bus.Subscribe(
              "infer",
              channel,
              [&delivered](const mould::comm::MessageEnvelope&) {
                delivered.fetch_add(1, std::memory_order_relaxed);
              })) {
        fail_handshake();
        close(w2p[1]);
        close(p2w[0]);
        _exit(43);
      }
      if (!bus.StartUnifiedSubscriberPump()) {
        fail_handshake();
        close(w2p[1]);
        close(p2w[0]);
        _exit(43);
      }
      for (std::uint32_t i = 0; i < kSplitRolesWarmupPublishCount; ++i) {
        const std::vector<std::uint8_t> payload{0x33, static_cast<std::uint8_t>(i)};
        if (!bus.Publish("broker", channel, payload)) {
          fail_handshake();
          close(w2p[1]);
          close(p2w[0]);
          _exit(44);
        }
      }
      if (!WaitAtomicUintAtLeast(&delivered, kSplitRolesWarmupPublishCount)) {
        fail_handshake();
        close(w2p[1]);
        close(p2w[0]);
        _exit(45);
      }
      const std::uint8_t ready = 1;
      if (!WriteFully(w2p[1], &ready, sizeof(ready))) {
        close(w2p[1]);
        close(p2w[0]);
        _exit(46);
      }
      std::uint8_t ack = 0;
      if (!ReadFully(p2w[0], &ack, sizeof(ack)) || ack != 1) {
        close(w2p[1]);
        close(p2w[0]);
        _exit(47);
      }
      bool ok_extra = true;
      for (std::uint32_t j = 0; j < 3U; ++j) {
        const std::vector<std::uint8_t> payload{0x44, static_cast<std::uint8_t>(j)};
        ok_extra = ok_extra && bus.Publish("broker", channel, payload);
      }
      extra_pub_ok = ok_extra ? 1 : 0;
      usleep(200 * 1000);
      const std::uint32_t final_count = delivered.load(std::memory_order_relaxed);
      (void)WriteFully(w2p[1], &final_count, sizeof(final_count));
      (void)WriteFully(w2p[1], &extra_pub_ok, sizeof(extra_pub_ok));
    }
    close(w2p[1]);
    close(p2w[0]);
    _exit(0);
  }
  close(w2p[1]);
  close(p2w[0]);
  std::uint8_t handshake = 0;
  if (!ReadFully(w2p[0], &handshake, sizeof(handshake))) {
    close(w2p[0]);
    close(p2w[1]);
    (void)waitpid(worker, nullptr, 0);
    control_plane.FinalizeUnlinkManagedSegments();
    return Check(false, "worker handshake read");
  }
  if (handshake != 1) {
    close(w2p[0]);
    close(p2w[1]);
    int st = 0;
    (void)waitpid(worker, &st, 0);
    control_plane.FinalizeUnlinkManagedSegments();
    return Check(WIFEXITED(st), "worker should exit after handshake failure");
  }
  const std::uint32_t swept =
      control_plane.TakeAllConsumersOfflineForProcessPid(
          static_cast<std::uint32_t>(worker), topology, slot_payload_bytes);
  if (!Check(swept == 1, "supervisor should offline one consumer slot")) {
    const std::uint8_t ack = 1;
    (void)WriteFully(p2w[1], &ack, sizeof(ack));
    close(w2p[0]);
    close(p2w[1]);
    (void)waitpid(worker, nullptr, 0);
    control_plane.FinalizeUnlinkManagedSegments();
    return false;
  }
  const std::uint8_t ack = 1;
  if (!Check(WriteFully(p2w[1], &ack, sizeof(ack)), "ack worker")) {
    close(w2p[0]);
    close(p2w[1]);
    (void)waitpid(worker, nullptr, 0);
    control_plane.FinalizeUnlinkManagedSegments();
    return false;
  }
  std::uint32_t final_delivered = 0;
  std::uint8_t extra_ok = 0;
  const bool tail_read =
      ReadFully(w2p[0], &final_delivered, sizeof(final_delivered)) &&
      ReadFully(w2p[0], &extra_ok, sizeof(extra_ok));
  close(w2p[0]);
  close(p2w[1]);
  int st = 0;
  if (!Check(waitpid(worker, &st, 0) == worker, "waitpid worker")) {
    control_plane.FinalizeUnlinkManagedSegments();
    return false;
  }
  control_plane.FinalizeUnlinkManagedSegments();
  return Check(
      tail_read && WIFEXITED(st) && WEXITSTATUS(st) == 0 && extra_ok == 1 &&
          final_delivered == kSplitRolesWarmupPublishCount,
      "after consumer offline: extra publishes succeed and subscriber gets no new deliveries");
}

bool TestAttachOnlyWithoutCreator_FailsTopology() {
  ShmBusRuntimeFinalizeSharedControlPlaneShm();
  ShmBusRuntimePreforkEnsureSharedControlPlane();
  auto plane = ShmBusRuntimeGetSharedControlPlane();
  if (!Check(static_cast<bool>(plane), "shared control plane should exist")) {
    return false;
  }

  mould::comm::MiddlewareConfig cfg;
  mould::comm::ShmBusRuntime bus(cfg);
  const std::string channel =
      "split_ut_atf_" + std::to_string(getpid()) + "_" +
      std::to_string(
          static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));

  mould::config::ChannelTopologyIndex topology;
  mould::config::ChannelTopologyEntry entry;
  entry.channel = channel;
  entry.producers = {"broker"};
  entry.consumers = {"infer"};
  entry.consumer_count = 1;
  topology.emplace(channel, entry);

  const std::string posix = mould::comm::BuildDeterministicShmName(
      mould::config::CanonicalShmChannelKey(entry));
  (void)shm_unlink(posix.c_str());

  const bool ok_fail = bus.SetChannelTopology(topology);
  ShmBusRuntimeFinalizeSharedControlPlaneShm();
  return Check(!ok_fail, "SetChannelTopology should fail when POSIX object is missing");
}

bool TestLaunchPlanFrozen_ChannelNamingStable() {
  const fs::path root = MakeUniqueRoot();
  WriteFile(
      root / "io.json",
      std::string(R"({
  "input_channel": { "infer.results": {"slot_payload_bytes": "64"} },
  "output_channel": { "broker.frames": {"slot_payload_bytes": "8"} }
})"));
  const std::string plan = std::string(R"({
  "modules": {
    "broker": {
      "module_name": "broker",
      "resource": {},
      "module_params": {},
      "io_channels_config_path": "io.json"
    }
  }
})");
  WriteFile(root / "launch_plan.txt", plan);

  mould::config::ParsedLaunchPlan a;
  mould::config::ParsedLaunchPlan b;
  std::string e1;
  std::string e2;
  const std::string path = (root / "launch_plan.txt").string();
  const bool ok_a = mould::config::ParseLaunchPlanFile(path, &a, &e1);
  const bool ok_b = mould::config::ParseLaunchPlanFile(path, &b, &e2);
  (void)std::filesystem::remove_all(root);
  if (!Check(ok_a && ok_b, "double parse")) {
    return false;
  }
  if (!Check(a.global_topology.size() == b.global_topology.size(), "topology size stable")) {
    return false;
  }
  std::unordered_set<std::string> keys_a;
  std::unordered_set<std::string> keys_b;
  for (const auto& [ch, ent] : a.global_topology) {
    (void)ch;
    keys_a.insert(mould::config::CanonicalShmChannelKey(ent));
  }
  for (const auto& [ch, ent] : b.global_topology) {
    (void)ch;
    keys_b.insert(mould::config::CanonicalShmChannelKey(ent));
  }
  return Check(keys_a == keys_b, "canonical shm keys stable across reparse");
}

}  // namespace

TEST(SplitShmBusControlRuntimeTest, ShmUnlinkPolicy_RuntimeVsFinalExit) {
  EXPECT_TRUE(TestShmUnlinkPolicy_RuntimeVsFinalExit());
}

TEST(SplitShmBusControlRuntimeTest, RegistryFrozen_RejectsUnknownChannelKey) {
  EXPECT_TRUE(TestRegistryFrozen_RejectsUnknownChannelKey());
}

TEST(SplitShmBusControlRuntimeTest, ConsumerCrash_OfflineAndReclaim) {
  EXPECT_TRUE(TestConsumerCrash_OfflineAndReclaim());
}

TEST(SplitShmBusControlRuntimeTest, PidReuse_OwnerEpochDisambiguates) {
  EXPECT_TRUE(TestPidReuse_OwnerEpochDisambiguates());
}

TEST(SplitShmBusControlRuntimeTest, PreforkSharedControlPlaneCowAddressStableAfterFork) {
  EXPECT_TRUE(TestPreforkSharedControlPlaneCowAddressStableAfterFork());
}

TEST(SplitShmBusControlRuntimeTest, SupervisorTakeConsumerOfflineOnLiveBus) {
  EXPECT_TRUE(TestSupervisorTakeConsumerOfflineOnLiveBus());
}

TEST(SplitShmBusControlRuntimeTest, ControlPlaneTakeAllConsumersOfflineForProcessPidOneChannel) {
  EXPECT_TRUE(TestControlPlane_TakeAllConsumersOfflineForProcessPid_one_channel());
}

TEST(SplitShmBusControlRuntimeTest, ControlPlaneTakeAllConsumersOfflineForProcessPidTwoChannels) {
  EXPECT_TRUE(TestControlPlane_TakeAllConsumersOfflineForProcessPid_two_channels());
}

TEST(SplitShmBusControlRuntimeTest, SplitControlParentDataChildBothPublisherAndConsumerOnline) {
  EXPECT_TRUE(TestSplitControlParent_DataChild_BothPublisherAndConsumerOnline());
}

TEST(SplitShmBusControlRuntimeTest, SplitControlParentDataChildProducerChildExitsConsumerStillLive) {
  EXPECT_TRUE(TestSplitControlParent_DataChild_ProducerChildExitsConsumerStillLive());
}

TEST(SplitShmBusControlRuntimeTest, SplitControlParentDataChildProducerLiveConsumerTakenOfflineBySupervisor) {
  EXPECT_TRUE(TestSplitControlParent_DataChild_ProducerLiveConsumerTakenOfflineBySupervisor());
}

TEST(SplitShmBusControlRuntimeTest, AttachOnlyWithoutCreator_FailsTopology) {
  EXPECT_TRUE(TestAttachOnlyWithoutCreator_FailsTopology());
}

TEST(SplitShmBusControlRuntimeTest, LaunchPlanFrozen_ChannelNamingStable) {
  EXPECT_TRUE(TestLaunchPlanFrozen_ChannelNamingStable());
}

TEST(SplitShmBusControlRuntimeTest, CompeteModeEndToEnd) {
  EXPECT_TRUE(TestCompeteDeliveryMode_EndToEndSingleHandlerPerPublish());
}

TEST(SplitShmBusControlRuntimeTest, BroadcastBackwardCompatibility) {
  EXPECT_TRUE(TestBroadcastDeliveryMode_TwoConsumersStillFanOut());
}

TEST(SplitShmBusControlRuntimeTest, TopologyParamParsing) {
  EXPECT_TRUE(TestTopologyResolve_DeliveryModeParams());
}

TEST(SplitShmBusControlRuntimeTest, CompeteMode_StressSameProcessHeavyPublishThroughBus) {
  EXPECT_TRUE(TestCompeteMode_StressSameProcessHeavyPublishThroughBus());
}

TEST(SplitShmBusControlRuntimeTest, MultiConsumerCompeteWithFork) {
  EXPECT_TRUE(TestCompeteMode_MultiprocessStressTwoConsumersInheritedFds());
}

TEST(SplitShmBusControlRuntimeSpec, ShmControlPlane_OfflineConsumerSlotsByPidOnly) {
  EXPECT_TRUE(TestControlPlane_TakeAllConsumersOfflineForProcessPid_two_channels());
}

TEST(SplitShmBusControlRuntimeSpec, ShmChannelLifecycle_UnlinkOnlyOnFinalMasterShutdown) {
  EXPECT_TRUE(TestShmUnlinkPolicy_RuntimeVsFinalExit());
}

TEST(SplitShmBusControlRuntimeSpec, SupervisorIntegration_CrashRestartLoopAndShmMembership) {
  EXPECT_TRUE(TestControlPlane_TakeAllConsumersOfflineForProcessPid_one_channel());
  EXPECT_TRUE(TestControlPlane_TakeAllConsumersOfflineForProcessPid_two_channels());
  EXPECT_TRUE(TestSplitControlParent_DataChild_ProducerLiveConsumerTakenOfflineBySupervisor());
}
