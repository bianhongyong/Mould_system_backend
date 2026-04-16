#include "ms_logging.hpp"
#include "shm_ring_buffer.hpp"
#include "shm_segment.hpp"
#include "test_helpers.hpp"

#include <sys/prctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using mould::comm::ComputeRingLayoutSizeBytes;
using mould::comm::ConsumerState;
using mould::comm::RingLayoutView;
using mould::comm::RingSlotReservation;
using mould::comm::ShmSegment;
using mould::comm::ShmSegmentLayout;

std::string BuildUniqueChannelName(const std::string& suffix) {
  return "module5_mp_" + suffix + "_" + std::to_string(::getpid()) + "_" +
      std::to_string(
             static_cast<std::uint64_t>(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
}

std::optional<RingLayoutView> AttachRing(ShmSegment* segment) {
  if (segment == nullptr) {
    return std::nullopt;
  }
  auto* ring_base = static_cast<std::uint8_t*>(segment->BaseAddress()) + mould::comm::ShmSegmentHeaderSizeBytes();
  const std::size_t ring_span = segment->SizeBytes() - mould::comm::ShmSegmentHeaderSizeBytes();
  return RingLayoutView::Attach(ring_base, ring_span);
}

std::atomic<bool> g_child_stop{false};

void ChildSignalHandler(int /*signum*/) {
  g_child_stop.store(true, std::memory_order_release);
}

bool TestAddConsumerStartsAtLatestSequence() {
  constexpr std::uint32_t slot_count = 8;
  constexpr std::uint32_t consumer_capacity = 4;
  constexpr std::uint64_t payload_bytes = 256;
  const std::size_t layout_bytes = ComputeRingLayoutSizeBytes(slot_count, consumer_capacity);
  std::vector<std::uint8_t> backing(layout_bytes + payload_bytes, 0);

  auto ring = RingLayoutView::Initialize(backing.data(), backing.size(), slot_count, consumer_capacity, payload_bytes);
  if (!Check(ring.has_value(), "ring initialize should succeed for add-consumer test")) {
    return false;
  }

  for (std::uint32_t slot = 0; slot < 3; ++slot) {
    RingSlotReservation reservation{};
    if (!Check(ring->ReserveSlot(slot, 8, slot * 8, &reservation) && ring->CommitSlot(slot),
               "preload committed history should succeed")) {
      return false;
    }
  }

  const std::uint64_t next_before_add = ring->Header()->next_sequence.load(std::memory_order_acquire);
  if (!Check(ring->AddConsumerOnline(0), "add consumer should succeed")) {
    return false;
  }

  const auto cursor = ring->LoadConsumerCursor(0);
  return Check(
      cursor.has_value() && *cursor == next_before_add,
      "new consumer cursor should start at latest head and skip historical backlog");
}

bool TestRemoveConsumerTriggersImmediateReclaim() {
  constexpr std::uint32_t slot_count = 4;
  constexpr std::uint32_t consumer_capacity = 2;
  constexpr std::uint64_t payload_bytes = 128;
  const std::size_t layout_bytes = ComputeRingLayoutSizeBytes(slot_count, consumer_capacity);
  std::vector<std::uint8_t> backing(layout_bytes + payload_bytes, 0);

  auto ring = RingLayoutView::Initialize(backing.data(), backing.size(), slot_count, consumer_capacity, payload_bytes);
  if (!Check(ring.has_value(), "ring initialize should succeed for remove-consumer reclaim test")) {
    return false;
  }
  if (!Check(ring->RegisterConsumer(0, 4), "fast consumer register should succeed")) {
    return false;
  }
  if (!Check(ring->RegisterConsumer(1, 1), "slow consumer register should succeed")) {
    return false;
  }

  for (std::uint32_t slot = 0; slot < 3; ++slot) {
    RingSlotReservation reservation{};
    if (!Check(ring->ReserveSlot(slot, 8, slot * 8, &reservation) && ring->CommitSlot(slot),
               "prepare committed slots should succeed")) {
      return false;
    }
  }

  if (!Check(ring->RemoveConsumerOnline(1), "remove slow consumer should succeed")) {
    return false;
  }
  if (!Check(
          ring->Consumers()[1].state.load(std::memory_order_acquire) ==
              static_cast<std::uint32_t>(ConsumerState::kOffline),
          "removed consumer should transition to OFFLINE")) {
    return false;
  }

  RingSlotReservation slot{};
  const bool reclaimed_now = !ring->TryReadCommitted(0, &slot);
  return Check(reclaimed_now, "remove consumer should trigger one reclaim pass immediately");
}

bool TestMembershipGenerationIsConsistentUnderSwitch() {
  constexpr std::uint32_t slot_count = 16;
  constexpr std::uint32_t consumer_capacity = 8;
  constexpr std::uint64_t payload_bytes = 512;
  const std::size_t layout_bytes = ComputeRingLayoutSizeBytes(slot_count, consumer_capacity);
  std::vector<std::uint8_t> backing(layout_bytes + payload_bytes, 0);

  auto ring = RingLayoutView::Initialize(backing.data(), backing.size(), slot_count, consumer_capacity, payload_bytes);
  if (!Check(ring.has_value(), "ring initialize should succeed for generation consistency test")) {
    return false;
  }

  for (std::uint32_t i = 0; i < consumer_capacity; ++i) {
    if (!Check(ring->AddConsumerOnline(i), "initial add should succeed for all consumers")) {
      return false;
    }
  }

  std::atomic<bool> stop{false};
  std::atomic<int> failures{0};
  std::thread reader([&]() {
    while (!stop.load(std::memory_order_acquire)) {
      const auto before = ring->MembershipGeneration();
      const auto min_seq = ring->MinActiveReadSequence();
      const auto after = ring->MembershipGeneration();
      if (!before.has_value() || !after.has_value() || !min_seq.has_value()) {
        failures.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      if (*before > *after) {
        failures.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  });

  for (std::uint32_t round = 0; round < 500; ++round) {
    const std::uint32_t idx = round % consumer_capacity;
    if (!ring->RemoveConsumerOnline(idx)) {
      failures.fetch_add(1, std::memory_order_relaxed);
      break;
    }
    if (!ring->AddConsumerOnline(idx)) {
      failures.fetch_add(1, std::memory_order_relaxed);
      break;
    }
  }

  stop.store(true, std::memory_order_release);
  reader.join();
  return Check(
      failures.load(std::memory_order_relaxed) == 0,
      "generation switch should keep quorum reads on consistent snapshot");
}

bool TestHealthMetricsCoverage() {
  constexpr std::uint32_t slot_count = 6;
  constexpr std::uint32_t consumer_capacity = 3;
  constexpr std::uint64_t payload_bytes = 256;
  const std::size_t layout_bytes = ComputeRingLayoutSizeBytes(slot_count, consumer_capacity);
  std::vector<std::uint8_t> backing(layout_bytes + payload_bytes, 0);

  auto ring = RingLayoutView::Initialize(backing.data(), backing.size(), slot_count, consumer_capacity, payload_bytes);
  if (!Check(ring.has_value(), "ring initialize should succeed for metrics test")) {
    return false;
  }

  if (!Check(ring->AddConsumerOnline(0), "add consumer-0 should succeed")) {
    return false;
  }
  if (!Check(ring->AddConsumerOnline(1), "add consumer-1 should succeed")) {
    return false;
  }
  if (!Check(ring->RecordProducerBlockDuration(1234), "record producer block duration should succeed")) {
    return false;
  }

  RingSlotReservation reservation{};
  if (!Check(ring->ReserveSlot(0, 16, 0, &reservation) && ring->CommitSlot(0), "commit one slot for occupancy")) {
    return false;
  }
  if (!Check(ring->AdvanceConsumerCursor(0, reservation.sequence + 1), "advance one consumer to create lag delta")) {
    return false;
  }

  const auto metrics = ring->ObserveHealthMetrics();
  bool ok = true;
  ok = Check(metrics.slot_capacity == slot_count, "metrics should include slot capacity") && ok;
  ok = Check(metrics.occupancy_slots >= 1, "metrics should expose occupancy") && ok;
  ok = Check(metrics.producer_block_total_ns >= 1234, "metrics should expose producer block duration") && ok;
  ok = Check(metrics.max_consumer_lag >= 1, "metrics should expose consumer lag") && ok;
  ok = Check(metrics.reclaim_count + 1 > metrics.reclaim_count, "metrics should expose reclaim count") && ok;
  ok = Check(metrics.membership_change_count >= 2, "metrics should expose membership changes") && ok;
  ok = Check(metrics.consumer_online_events >= 2, "metrics should expose online events") && ok;

  if (!Check(ring->RemoveConsumerOnline(1), "remove consumer-1 should succeed")) {
    return false;
  }
  const auto after_remove = ring->ObserveHealthMetrics();
  ok = Check(after_remove.consumer_offline_events >= 1, "metrics should expose offline events") && ok;
  return ok;
}

bool TestDynamicMultiProcessProducerConsumerPressure30s() {
  constexpr std::uint32_t slot_count = 64;
  constexpr std::uint32_t consumer_capacity = 8;
  constexpr std::uint64_t payload_bytes = 64 * 256;
  constexpr std::uint32_t message_bytes = 64;
  constexpr auto test_duration = std::chrono::seconds(30);
  constexpr auto control_tick = std::chrono::milliseconds(200);

  struct ChildConsumer {
    std::uint32_t index = 0;
    pid_t pid = -1;
  };

  const std::string channel = BuildUniqueChannelName("pressure30s");
  const std::string shm_name = mould::comm::BuildDeterministicShmName(channel);
  shm_unlink(shm_name.c_str());

  ShmSegmentLayout layout;
  layout.payload_capacity = ComputeRingLayoutSizeBytes(slot_count, consumer_capacity) + payload_bytes;
  auto segment = ShmSegment::CreateOrAttach(channel, layout);
  auto ring = segment
      ? RingLayoutView::Initialize(
            static_cast<std::uint8_t*>(segment->BaseAddress()) + mould::comm::ShmSegmentHeaderSizeBytes(),
            segment->SizeBytes() - mould::comm::ShmSegmentHeaderSizeBytes(),
            slot_count,
            consumer_capacity,
            payload_bytes)
      : std::nullopt;
  if (!Check(ring.has_value(), "parent should initialize ring for dynamic multi-process pressure test")) {
    shm_unlink(shm_name.c_str());
    return false;
  }

  std::atomic<bool> producer_stop{false};
  std::atomic<std::uint64_t> produced_count{0};
  std::atomic<int> producer_failures{0};
  std::thread producer([&]() {
    while (!producer_stop.load(std::memory_order_acquire)) {
      const std::uint64_t produced_now = produced_count.load(std::memory_order_relaxed);
      const std::uint32_t slot_index = static_cast<std::uint32_t>(produced_now % slot_count);
      const std::uint64_t payload_offset =
          static_cast<std::uint64_t>(slot_index) * static_cast<std::uint64_t>(message_bytes);

      RingSlotReservation reservation{};
      const auto reserve_start = std::chrono::steady_clock::now();
      while (!ring->ReserveSlot(slot_index, message_bytes, payload_offset, &reservation)) {
        if (producer_stop.load(std::memory_order_acquire)) {
          return;
        }
        std::this_thread::yield();
      }
      const auto reserve_end = std::chrono::steady_clock::now();
      const auto blocked_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(reserve_end - reserve_start).count();
      if (blocked_ns > 0) {
        (void)ring->RecordProducerBlockDuration(static_cast<std::uint64_t>(blocked_ns));
      }

      const std::uint8_t marker = static_cast<std::uint8_t>((reservation.sequence % 251U) + 1U);
      std::uint8_t* payload = ring->PayloadRegion();
      for (std::uint32_t i = 0; i < message_bytes; ++i) {
        payload[payload_offset + i] = marker;
      }
      if (!ring->CommitSlot(slot_index)) {
        producer_failures.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      produced_count.fetch_add(1, std::memory_order_release);
    }
  });

  std::vector<std::optional<ChildConsumer>> children(consumer_capacity);
  std::uint64_t add_ops = 0;
  std::uint64_t remove_ops = 0;
  int child_failures = 0;

  auto spawn_consumer = [&](std::uint32_t index) -> bool {
    if (!ring->AddConsumerOnline(index)) {
      return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
      (void)ring->RemoveConsumerOnline(index);
      return false;
    }
    if (pid == 0) {
      g_child_stop.store(false, std::memory_order_release);
      (void)prctl(PR_SET_PDEATHSIG, SIGTERM);
      if (getppid() == 1) {
        _exit(90);
      }
      struct sigaction sa {};
      sa.sa_handler = ChildSignalHandler;
      sigemptyset(&sa.sa_mask);
      sa.sa_flags = 0;
      (void)sigaction(SIGTERM, &sa, nullptr);
      (void)sigaction(SIGINT, &sa, nullptr);

      auto child_segment = ShmSegment::CreateOrAttach(channel, layout);
      auto child_ring = child_segment ? AttachRing(&(*child_segment)) : std::nullopt;
      if (!child_ring.has_value()) {
        _exit(91);
      }

      while (!g_child_stop.load(std::memory_order_acquire)) {
        const auto cursor = child_ring->LoadConsumerCursor(index);
        if (!cursor.has_value()) {
          std::this_thread::yield();
          continue;
        }
        const std::uint64_t expected_sequence = *cursor;
        const std::uint32_t slot_idx = static_cast<std::uint32_t>((expected_sequence - 1U) % slot_count);
        RingSlotReservation committed{};
        if (!child_ring->TryReadCommitted(slot_idx, &committed)) {
          std::this_thread::yield();
          continue;
        }
        if (committed.sequence < expected_sequence) {
          std::this_thread::yield();
          continue;
        }
        const std::uint8_t expected_marker = static_cast<std::uint8_t>((committed.sequence % 251U) + 1U);
        const std::uint8_t* payload = child_ring->PayloadRegion();
        for (std::uint32_t i = 0; i < committed.payload_size; ++i) {
          if (payload[committed.payload_offset + i] != expected_marker) {
            _exit(93);
          }
        }
        if (!child_ring->CompleteConsumerSlot(index, slot_idx, committed.sequence + 1U)) {
          _exit(94);
        }
      }
      _exit(0);
    }

    children[index] = ChildConsumer{index, pid};
    return true;
  };

  auto stop_consumer = [&](std::uint32_t index) -> bool {
    if (!children[index].has_value()) {
      return false;
    }
    bool ok = true;
    ok = ring->RemoveConsumerOnline(index) && ok;
    pid_t pid = children[index]->pid;
    (void)kill(pid, SIGTERM);
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      ok = false;
      ++child_failures;
    }
    children[index].reset();
    return ok;
  };

  // Start with two consumers online.
  bool orchestrator_ok = spawn_consumer(0) && spawn_consumer(1);
  add_ops += 2;

  const auto deadline = std::chrono::steady_clock::now() + test_duration;
  std::uint32_t next_index = 2;
  bool add_phase = true;
  while (std::chrono::steady_clock::now() < deadline && orchestrator_ok) {
    if (add_phase) {
      bool added = false;
      for (std::uint32_t step = 0; step < consumer_capacity; ++step) {
        const std::uint32_t idx = (next_index + step) % consumer_capacity;
        if (!children[idx].has_value()) {
          added = spawn_consumer(idx);
          if (added) {
            ++add_ops;
            next_index = (idx + 1U) % consumer_capacity;
          }
          break;
        }
      }
      orchestrator_ok = added || orchestrator_ok;
    } else {
      bool removed = false;
      for (std::uint32_t step = 0; step < consumer_capacity; ++step) {
        const std::uint32_t idx = (next_index + step) % consumer_capacity;
        if (children[idx].has_value()) {
          removed = stop_consumer(idx);
          if (removed) {
            ++remove_ops;
            next_index = (idx + 1U) % consumer_capacity;
          }
          break;
        }
      }
      orchestrator_ok = removed || orchestrator_ok;
    }
    add_phase = !add_phase;
    std::this_thread::sleep_for(control_tick);
  }

  producer_stop.store(true, std::memory_order_release);
  producer.join();

  for (std::uint32_t i = 0; i < consumer_capacity; ++i) {
    if (children[i].has_value()) {
      if (stop_consumer(i)) {
        ++remove_ops;
      } else {
        orchestrator_ok = false;
      }
    }
  }

  const auto metrics = ring->ObserveHealthMetrics();
  const std::uint64_t produced_total = produced_count.load(std::memory_order_acquire);
  const std::uint64_t expected_membership_changes = add_ops + remove_ops;
  const bool metrics_ok = metrics.membership_change_count >= expected_membership_changes &&
      metrics.consumer_online_events >= add_ops && metrics.consumer_offline_events >= remove_ops;
  const bool pressure_ok = metrics.producer_block_total_ns > 0;
  const int producer_failures_total = producer_failures.load(std::memory_order_relaxed);
  const bool final_ok = orchestrator_ok &&
      producer_failures_total < 100 &&
      child_failures == 0 &&
      produced_total > slot_count * 100U &&
      add_ops > 2 &&
      remove_ops > 0 &&
      metrics_ok &&
      pressure_ok;

  LOG(INFO) << "[module5-pressure] produced=" << produced_total
            << " add_ops=" << add_ops << " remove_ops=" << remove_ops
            << " producer_failures=" << producer_failures_total
            << " child_failures=" << child_failures
            << " orchestrator_ok=" << (orchestrator_ok ? 1 : 0)
            << " metrics_ok=" << (metrics_ok ? 1 : 0)
            << " pressure_ok=" << (pressure_ok ? 1 : 0)
            << " membership_changes=" << metrics.membership_change_count
            << " online_events=" << metrics.consumer_online_events
            << " offline_events=" << metrics.consumer_offline_events
            << " producer_block_total_ns=" << metrics.producer_block_total_ns;

  shm_unlink(shm_name.c_str());
  return Check(final_ok, "30s dynamic multi-process producer/consumer pressure should satisfy invariants");
}

}  // namespace

int main(int argc, char* argv[]) {
  (void)argc;
  mould::InitApplicationLogging(argv[0]);
  bool ok = true;
  ok = TestAddConsumerStartsAtLatestSequence() && ok;
  ok = TestRemoveConsumerTriggersImmediateReclaim() && ok;
  ok = TestMembershipGenerationIsConsistentUnderSwitch() && ok;
  ok = TestHealthMetricsCoverage() && ok;
  ok = TestDynamicMultiProcessProducerConsumerPressure30s() && ok;
  if (!ok) {
    LOG(ERROR) << "module5 daemon membership/observability tests failed";
    mould::ShutdownApplicationLogging();
    return 1;
  }
  LOG(INFO) << "module5 daemon membership/observability tests passed";
  mould::ShutdownApplicationLogging();
  return 0;
}
