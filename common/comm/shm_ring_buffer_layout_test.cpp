#include "ms_logging.hpp"
#include "shm_ring_buffer.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using mould::comm::ComputeRingLayoutSizeBytes;
using mould::comm::RingLayoutView;
using mould::comm::RingSlotReservation;
using mould::comm::SlotState;

bool TestInitializeAndAttachLayout() {
  constexpr std::uint32_t kSlotCount = 8;
  constexpr std::uint32_t kConsumerCapacity = 4;
  constexpr std::uint64_t kPayloadBytes = 256;
  const std::size_t layout_bytes = ComputeRingLayoutSizeBytes(kSlotCount, kConsumerCapacity);
  std::vector<std::uint8_t> backing(layout_bytes + kPayloadBytes, 0);

  auto owner = RingLayoutView::Initialize(
      backing.data(),
      backing.size(),
      kSlotCount,
      kConsumerCapacity,
      kPayloadBytes);
  if (!Check(owner.has_value(), "initialize should succeed for valid ring layout")) {
    return false;
  }

  auto attached = RingLayoutView::Attach(backing.data(), backing.size());
  if (!Check(attached.has_value(), "attach should accept matching layout version")) {
    return false;
  }

  return Check(
      attached->Header()->slot_count == kSlotCount &&
          attached->Header()->consumer_capacity == kConsumerCapacity,
      "attached view should expose configured fixed layout");
}

bool TestAttachRejectsIncompatibleVersion() {
  constexpr std::uint32_t kSlotCount = 4;
  constexpr std::uint32_t kConsumerCapacity = 2;
  constexpr std::uint64_t kPayloadBytes = 128;
  const std::size_t layout_bytes = ComputeRingLayoutSizeBytes(kSlotCount, kConsumerCapacity);
  std::vector<std::uint8_t> backing(layout_bytes + kPayloadBytes, 0);

  auto owner = RingLayoutView::Initialize(
      backing.data(),
      backing.size(),
      kSlotCount,
      kConsumerCapacity,
      kPayloadBytes);
  if (!Check(owner.has_value(), "initialize should succeed before mismatch attach")) {
    return false;
  }

  auto mismatch = RingLayoutView::Attach(backing.data(), backing.size(), owner->Header()->version + 1);
  return Check(!mismatch.has_value(), "attach should fail when ring layout version mismatches");
}

bool TestTwoPhaseCommitVisibility() {
  constexpr std::uint32_t kSlotCount = 4;
  constexpr std::uint32_t kConsumerCapacity = 2;
  constexpr std::uint64_t kPayloadBytes = 128;
  const std::size_t layout_bytes = ComputeRingLayoutSizeBytes(kSlotCount, kConsumerCapacity);
  std::vector<std::uint8_t> backing(layout_bytes + kPayloadBytes, 0);

  auto ring = RingLayoutView::Initialize(
      backing.data(),
      backing.size(),
      kSlotCount,
      kConsumerCapacity,
      kPayloadBytes);
  if (!Check(ring.has_value(), "initialize should succeed for commit visibility test")) {
    return false;
  }

  RingSlotReservation reserved{};
  if (!Check(ring->ReserveSlot(0, 32, 0, &reserved), "reserve should succeed for empty slot")) {
    return false;
  }

  RingSlotReservation read_before_commit{};
  if (!Check(!ring->TryReadCommitted(0, &read_before_commit), "consumer should not read slot before commit")) {
    return false;
  }
  if (!Check(
          ring->Slots()[0].state.load(std::memory_order_acquire) ==
              static_cast<std::uint32_t>(SlotState::kReserved),
          "reserved slot must stay non-committed before publish")) {
    return false;
  }

  if (!Check(ring->CommitSlot(0), "commit should transition slot to committed")) {
    return false;
  }

  RingSlotReservation read_after_commit{};
  if (!Check(ring->TryReadCommitted(0, &read_after_commit), "consumer should read slot after commit")) {
    return false;
  }
  if (!Check(
          read_after_commit.sequence == reserved.sequence &&
              read_after_commit.payload_size == reserved.payload_size,
          "committed metadata should match reserved metadata")) {
    return false;
  }

  if (!Check(ring->ResetSlot(0), "reset should release slot back to empty")) {
    return false;
  }
  return Check(
      ring->Slots()[0].state.load(std::memory_order_acquire) ==
          static_cast<std::uint32_t>(SlotState::kEmpty),
      "reset slot should become empty");
}

bool TestReserveRejectsInvalidParameters() {
  constexpr std::uint32_t kSlotCount = 2;
  constexpr std::uint32_t kConsumerCapacity = 1;
  constexpr std::uint64_t kPayloadBytes = 32;
  const std::size_t layout_bytes = ComputeRingLayoutSizeBytes(kSlotCount, kConsumerCapacity);
  std::vector<std::uint8_t> backing(layout_bytes + kPayloadBytes, 0);

  auto ring = RingLayoutView::Initialize(
      backing.data(),
      backing.size(),
      kSlotCount,
      kConsumerCapacity,
      kPayloadBytes);
  if (!Check(ring.has_value(), "initialize should succeed for invalid parameter checks")) {
    return false;
  }

  RingSlotReservation out{};
  if (!Check(!ring->ReserveSlot(0, 0, 0, &out), "reserve should reject zero payload size")) {
    return false;
  }
  if (!Check(
          !ring->ReserveSlot(0, 16, kPayloadBytes - 8, &out),
          "reserve should reject payload range crossing payload region")) {
    return false;
  }
  return Check(
      !ring->ReserveSlot(kSlotCount, 8, 0, &out),
      "reserve should reject out-of-range slot index");
}

bool TestInvalidStateTransitions() {
  constexpr std::uint32_t kSlotCount = 2;
  constexpr std::uint32_t kConsumerCapacity = 1;
  constexpr std::uint64_t kPayloadBytes = 64;
  const std::size_t layout_bytes = ComputeRingLayoutSizeBytes(kSlotCount, kConsumerCapacity);
  std::vector<std::uint8_t> backing(layout_bytes + kPayloadBytes, 0);

  auto ring = RingLayoutView::Initialize(
      backing.data(),
      backing.size(),
      kSlotCount,
      kConsumerCapacity,
      kPayloadBytes);
  if (!Check(ring.has_value(), "initialize should succeed for invalid transition checks")) {
    return false;
  }

  if (!Check(!ring->CommitSlot(0), "commit should fail when slot is empty")) {
    return false;
  }

  RingSlotReservation out{};
  if (!Check(ring->ReserveSlot(0, 8, 0, &out), "reserve should succeed for empty slot")) {
    return false;
  }
  if (!Check(!ring->ReserveSlot(0, 8, 8, &out), "reserve should fail for non-empty slot")) {
    return false;
  }
  if (!Check(ring->CommitSlot(0), "first commit should succeed")) {
    return false;
  }
  if (!Check(!ring->CommitSlot(0), "second commit should fail for committed slot")) {
    return false;
  }
  if (!Check(ring->ResetSlot(0), "reset should succeed after commit")) {
    return false;
  }
  return Check(ring->ReserveSlot(0, 8, 0, &out), "slot should be reusable after reset");
}

bool TestAttachRejectsCorruptedHeader() {
  constexpr std::uint32_t kSlotCount = 2;
  constexpr std::uint32_t kConsumerCapacity = 1;
  constexpr std::uint64_t kPayloadBytes = 64;
  const std::size_t layout_bytes = ComputeRingLayoutSizeBytes(kSlotCount, kConsumerCapacity);
  std::vector<std::uint8_t> backing(layout_bytes + kPayloadBytes, 0);

  auto ring = RingLayoutView::Initialize(
      backing.data(),
      backing.size(),
      kSlotCount,
      kConsumerCapacity,
      kPayloadBytes);
  if (!Check(ring.has_value(), "initialize should succeed before corruption checks")) {
    return false;
  }

  ring->Header()->magic = 0;
  if (!Check(
          !RingLayoutView::Attach(backing.data(), backing.size()).has_value(),
          "attach should reject mismatched magic")) {
    return false;
  }

  ring->Header()->magic = mould::comm::kRingLayoutMagic;
  ring->Header()->payload_region_bytes = static_cast<std::uint64_t>(backing.size());
  return Check(
      !RingLayoutView::Attach(backing.data(), backing.size()).has_value(),
      "attach should reject incompatible header payload span");
}

bool TestSequenceMonotonicAcrossSlots() {
  constexpr std::uint32_t kSlotCount = 4;
  constexpr std::uint32_t kConsumerCapacity = 1;
  constexpr std::uint64_t kPayloadBytes = 128;
  const std::size_t layout_bytes = ComputeRingLayoutSizeBytes(kSlotCount, kConsumerCapacity);
  std::vector<std::uint8_t> backing(layout_bytes + kPayloadBytes, 0);

  auto ring = RingLayoutView::Initialize(
      backing.data(),
      backing.size(),
      kSlotCount,
      kConsumerCapacity,
      kPayloadBytes);
  if (!Check(ring.has_value(), "initialize should succeed for sequence monotonic checks")) {
    return false;
  }

  RingSlotReservation first{};
  RingSlotReservation second{};
  if (!Check(ring->ReserveSlot(0, 16, 0, &first), "first slot reserve should succeed")) {
    return false;
  }
  if (!Check(ring->ReserveSlot(1, 16, 16, &second), "second slot reserve should succeed")) {
    return false;
  }
  return Check(second.sequence > first.sequence, "sequence should increase across different slot indexes");
}

bool TestExplicitCommitAndReserveFailureSemantics() {
  constexpr std::uint32_t kSlotCount = 1;
  constexpr std::uint32_t kConsumerCapacity = 1;
  constexpr std::uint64_t kPayloadBytes = 64;
  const std::size_t layout_bytes = ComputeRingLayoutSizeBytes(kSlotCount, kConsumerCapacity);
  std::vector<std::uint8_t> backing(layout_bytes + kPayloadBytes, 0);

  auto ring = RingLayoutView::Initialize(
      backing.data(),
      backing.size(),
      kSlotCount,
      kConsumerCapacity,
      kPayloadBytes);
  if (!Check(ring.has_value(), "initialize should succeed for explicit failure semantics")) {
    return false;
  }

  // 未 reserve 直接 commit 必须失败。
  if (!Check(!ring->CommitSlot(0), "commit without reserve should fail")) {
    return false;
  }

  RingSlotReservation first{};
  if (!Check(ring->ReserveSlot(0, 8, 0, &first), "initial reserve should succeed")) {
    return false;
  }

  // 非 Empty 状态重复 reserve 必须失败。
  RingSlotReservation duplicate_reserve{};
  if (!Check(!ring->ReserveSlot(0, 8, 8, &duplicate_reserve), "reserve on non-empty slot should fail")) {
    return false;
  }

  if (!Check(ring->CommitSlot(0), "first commit should succeed after reserve")) {
    return false;
  }

  // 重复 commit 必须失败。
  return Check(!ring->CommitSlot(0), "repeated commit should fail");
}

bool TestConcurrentProducersReserveCommitStress() {
  constexpr std::uint32_t kSlotCount = 8;
  constexpr std::uint32_t kConsumerCapacity = 1;
  constexpr std::uint64_t kPayloadBytes = 512;
  constexpr int kThreads = 8;
  constexpr int kIterationsPerThread = 2000;
  const std::size_t layout_bytes = ComputeRingLayoutSizeBytes(kSlotCount, kConsumerCapacity);
  std::vector<std::uint8_t> backing(layout_bytes + kPayloadBytes, 0);

  auto ring = RingLayoutView::Initialize(
      backing.data(),
      backing.size(),
      kSlotCount,
      kConsumerCapacity,
      kPayloadBytes);
  if (!Check(ring.has_value(), "initialize should succeed for concurrent producer stress")) {
    return false;
  }

  const int total_messages = kThreads * kIterationsPerThread;
  std::vector<std::atomic<std::uint8_t>> seen(static_cast<std::size_t>(total_messages + 1));
  for (auto& flag : seen) {
    flag.store(0, std::memory_order_relaxed);
  }
  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&, t]() {
      const std::uint32_t slot = static_cast<std::uint32_t>(t);
      const std::uint64_t offset = static_cast<std::uint64_t>(t * 8);
      for (int i = 0; i < kIterationsPerThread; ++i) {
        RingSlotReservation reservation{};
        if (!ring->ReserveSlot(slot, 8, offset, &reservation)) {
          failures.fetch_add(1, std::memory_order_relaxed);
          break;
        }
        std::uint8_t* payload = ring->PayloadRegion();
        const std::uint8_t marker = static_cast<std::uint8_t>((reservation.sequence % 251U) + 1U);
        for (int byte = 0; byte < 8; ++byte) {
          payload[offset + static_cast<std::uint64_t>(byte)] = marker;
        }
        if (!ring->CommitSlot(slot)) {
          failures.fetch_add(1, std::memory_order_relaxed);
          break;
        }

        RingSlotReservation committed{};
        if (!ring->TryReadCommitted(slot, &committed) || committed.sequence != reservation.sequence) {
          failures.fetch_add(1, std::memory_order_relaxed);
          break;
        }

        if (committed.sequence == 0 || committed.sequence > static_cast<std::uint64_t>(total_messages)) {
          failures.fetch_add(1, std::memory_order_relaxed);
          break;
        }
        const std::size_t seen_idx = static_cast<std::size_t>(committed.sequence);
        std::uint8_t expected = 0;
        if (!seen[seen_idx].compare_exchange_strong(expected, 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
          failures.fetch_add(1, std::memory_order_relaxed);
          break;
        }
        if (!ring->ResetSlot(slot)) {
          failures.fetch_add(1, std::memory_order_relaxed);
          break;
        }
      }
    });
  }

  for (auto& worker : workers) {
    worker.join();
  }

  if (!Check(failures.load(std::memory_order_relaxed) == 0, "concurrent producers should not observe state corruption")) {
    return false;
  }
  for (int seq = 1; seq <= total_messages; ++seq) {
    if (!Check(
            seen[static_cast<std::size_t>(seq)].load(std::memory_order_acquire) == 1,
            "all produced sequences should be unique and observed exactly once")) {
      return false;
    }
  }
  return true;
}

bool TestVisibilityUnderProducerConsumerPressure() {
  constexpr std::uint32_t kSlotCount = 1;
  constexpr std::uint32_t kConsumerCapacity = 1;
  constexpr std::uint64_t kPayloadBytes = 64;
  constexpr int kIterations = 8000;
  constexpr std::uint32_t kMessageBytes = 32;
  const std::size_t layout_bytes = ComputeRingLayoutSizeBytes(kSlotCount, kConsumerCapacity);
  std::vector<std::uint8_t> backing(layout_bytes + kPayloadBytes, 0);

  auto ring = RingLayoutView::Initialize(
      backing.data(),
      backing.size(),
      kSlotCount,
      kConsumerCapacity,
      kPayloadBytes);
  if (!Check(ring.has_value(), "initialize should succeed for visibility pressure")) {
    return false;
  }

  std::atomic<bool> producer_done{false};
  std::atomic<int> failures{0};
  std::atomic<std::uint64_t> last_sequence{0};

  std::thread producer([&]() {
    for (int i = 0; i < kIterations; ++i) {
      RingSlotReservation reservation{};
      while (!ring->ReserveSlot(0, kMessageBytes, 0, &reservation)) {
        std::this_thread::yield();
      }

      std::uint8_t* payload = ring->PayloadRegion();
      const std::uint8_t marker = static_cast<std::uint8_t>((reservation.sequence % 251U) + 1U);
      for (std::uint32_t b = 0; b < kMessageBytes; ++b) {
        payload[b] = marker;
      }

      if (!ring->CommitSlot(0)) {
        failures.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::thread consumer([&]() {
    int consumed = 0;
    while (consumed < kIterations) {
      RingSlotReservation committed{};
      if (!ring->TryReadCommitted(0, &committed)) {
        if (producer_done.load(std::memory_order_acquire) && failures.load(std::memory_order_relaxed) > 0) {
          break;
        }
        std::this_thread::yield();
        continue;
      }

      const std::uint8_t* payload = ring->PayloadRegion();
      const std::uint8_t expected_marker = static_cast<std::uint8_t>((committed.sequence % 251U) + 1U);
      for (std::uint32_t b = 0; b < committed.payload_size; ++b) {
        if (payload[b] != expected_marker) {
          failures.fetch_add(1, std::memory_order_relaxed);
          return;
        }
      }

      const std::uint64_t prev = last_sequence.load(std::memory_order_acquire);
      if (committed.sequence <= prev) {
        failures.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      last_sequence.store(committed.sequence, std::memory_order_release);

      if (!ring->ResetSlot(0)) {
        failures.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      ++consumed;
    }
  });

  producer.join();
  consumer.join();

  return Check(
      failures.load(std::memory_order_relaxed) == 0 &&
          last_sequence.load(std::memory_order_acquire) == static_cast<std::uint64_t>(kIterations),
      "producer/consumer pressure should not observe torn payload or sequence regression");
}

bool TestBoundaryCapacityConditions() {
  constexpr std::uint32_t kSmallSlotCount = 1;
  constexpr std::uint32_t kSmallConsumerCapacity = 1;
  constexpr std::uint64_t kSmallPayloadBytes = 1;
  const std::size_t small_layout = ComputeRingLayoutSizeBytes(kSmallSlotCount, kSmallConsumerCapacity);

  std::vector<std::uint8_t> too_small((small_layout + kSmallPayloadBytes) - 1U, 0);
  auto small_fail = RingLayoutView::Initialize(
      too_small.data(),
      too_small.size(),
      kSmallSlotCount,
      kSmallConsumerCapacity,
      kSmallPayloadBytes);
  if (!Check(!small_fail.has_value(), "initialize should fail when total size is below exact minimum")) {
    return false;
  }

  std::vector<std::uint8_t> exact(small_layout + kSmallPayloadBytes, 0);
  auto small_ok = RingLayoutView::Initialize(
      exact.data(),
      exact.size(),
      kSmallSlotCount,
      kSmallConsumerCapacity,
      kSmallPayloadBytes);
  if (!Check(small_ok.has_value(), "initialize should succeed at exact minimum size")) {
    return false;
  }

  RingSlotReservation one_byte{};
  if (!Check(small_ok->ReserveSlot(0, 1, 0, &one_byte), "single-byte payload should fit in minimal payload region")) {
    return false;
  }
  if (!Check(small_ok->CommitSlot(0), "single-byte payload commit should succeed")) {
    return false;
  }

  constexpr std::uint32_t kLargeSlotCount = 16384;
  constexpr std::uint32_t kLargeConsumerCapacity = 2;
  constexpr std::uint64_t kLargePayloadBytes = 8;
  const std::size_t large_layout = ComputeRingLayoutSizeBytes(kLargeSlotCount, kLargeConsumerCapacity);
  std::vector<std::uint8_t> large(large_layout + kLargePayloadBytes, 0);
  auto large_ok = RingLayoutView::Initialize(
      large.data(),
      large.size(),
      kLargeSlotCount,
      kLargeConsumerCapacity,
      kLargePayloadBytes);
  if (!Check(large_ok.has_value(), "initialize should support high slot count within bounded memory")) {
    return false;
  }

  RingSlotReservation tail_slot{};
  return Check(
      large_ok->ReserveSlot(kLargeSlotCount - 1U, 1, 0, &tail_slot),
      "highest valid slot index should remain reservable at large slot_count");
}

bool TestBacklogWithSlowConsumerNoOverwrite() {
  constexpr std::uint32_t kSlotCount = 16;
  constexpr std::uint32_t kConsumerCapacity = 1;
  constexpr std::uint64_t kPayloadBytes = 1024;
  constexpr std::uint32_t kMessageBytes = 32;
  constexpr int kIterations = 3000;
  const std::size_t layout_bytes = ComputeRingLayoutSizeBytes(kSlotCount, kConsumerCapacity);
  std::vector<std::uint8_t> backing(layout_bytes + kPayloadBytes, 0);

  auto ring = RingLayoutView::Initialize(
      backing.data(),
      backing.size(),
      kSlotCount,
      kConsumerCapacity,
      kPayloadBytes);
  if (!Check(ring.has_value(), "initialize should succeed for slow-consumer backlog test")) {
    return false;
  }

  std::atomic<int> produced{0};
  std::atomic<int> consumed{0};
  std::atomic<int> failures{0};
  std::atomic<int> producer_reserve_failures{0};
  std::atomic<int> producer_max_consecutive_reserve_failures{0};
  std::atomic<std::uint64_t> last_consumed_sequence{0};

  std::thread producer([&]() {
    int local_produced = 0;
    int local_consecutive_reserve_failures = 0;
    int local_max_consecutive_reserve_failures = 0;
    while (local_produced < kIterations) {
      const std::uint32_t slot = static_cast<std::uint32_t>(local_produced % static_cast<int>(kSlotCount));
      const std::uint64_t payload_offset =
          static_cast<std::uint64_t>(slot) * static_cast<std::uint64_t>(kMessageBytes);

      RingSlotReservation reservation{};
      if (!ring->ReserveSlot(slot, kMessageBytes, payload_offset, &reservation)) {
        producer_reserve_failures.fetch_add(1, std::memory_order_relaxed);
        ++local_consecutive_reserve_failures;
        if (local_consecutive_reserve_failures > local_max_consecutive_reserve_failures) {
          local_max_consecutive_reserve_failures = local_consecutive_reserve_failures;
        }
        std::this_thread::yield();
        continue;
      }
      local_consecutive_reserve_failures = 0;

      std::uint8_t* payload = ring->PayloadRegion();
      const std::uint8_t marker = static_cast<std::uint8_t>((reservation.sequence % 251U) + 1U);
      for (std::uint32_t i = 0; i < kMessageBytes; ++i) {
        payload[payload_offset + i] = marker;
      }

      if (!ring->CommitSlot(slot)) {
        failures.fetch_add(1, std::memory_order_relaxed);
        return;
      }

      ++local_produced;
      produced.store(local_produced, std::memory_order_release);
    }
    producer_max_consecutive_reserve_failures.store(local_max_consecutive_reserve_failures, std::memory_order_release);
  });

  std::thread consumer([&]() {
    int local_consumed = 0;
    while (local_consumed < kIterations) {
      bool made_progress = false;
      for (std::uint32_t slot = 0; slot < kSlotCount; ++slot) {
        RingSlotReservation committed{};
        if (!ring->TryReadCommitted(slot, &committed)) {
          continue;
        }

        const std::uint8_t* payload = ring->PayloadRegion();
        const std::uint8_t expected_marker = static_cast<std::uint8_t>((committed.sequence % 251U) + 1U);
        for (std::uint32_t i = 0; i < committed.payload_size; ++i) {
          if (payload[committed.payload_offset + i] != expected_marker) {
            failures.fetch_add(1, std::memory_order_relaxed);
            return;
          }
        }

        const std::uint64_t previous = last_consumed_sequence.load(std::memory_order_acquire);
        if (committed.sequence <= previous) {
          failures.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        last_consumed_sequence.store(committed.sequence, std::memory_order_release);

        if (!ring->ResetSlot(slot)) {
          failures.fetch_add(1, std::memory_order_relaxed);
          return;
        }

        ++local_consumed;
        consumed.store(local_consumed, std::memory_order_release);
        made_progress = true;

        // Deliberately slow down consumption to keep backlog pressure.
        std::this_thread::sleep_for(std::chrono::microseconds(40));
        if (local_consumed >= kIterations) {
          break;
        }
      }

      if (!made_progress) {
        if (produced.load(std::memory_order_acquire) >= kIterations &&
            consumed.load(std::memory_order_acquire) >= kIterations) {
          break;
        }
        std::this_thread::yield();
      }
    }
  });

  producer.join();
  consumer.join();

  LOG(INFO) << "[backlog-stats] producer_reserve_failures="
             << producer_reserve_failures.load(std::memory_order_acquire)
             << " max_consecutive_reserve_failures="
             << producer_max_consecutive_reserve_failures.load(std::memory_order_acquire)
             << " produced=" << produced.load(std::memory_order_acquire)
             << " consumed=" << consumed.load(std::memory_order_acquire);

  return Check(
      failures.load(std::memory_order_relaxed) == 0 &&
          produced.load(std::memory_order_acquire) == kIterations &&
          consumed.load(std::memory_order_acquire) == kIterations,
      "slow-consumer backlog should not allow overwrite before consumption");
}

}  // namespace

int main(int argc, char* argv[]) {
  (void)argc;
  mould::InitApplicationLogging(argv[0]);
  bool ok = true;
  ok = TestInitializeAndAttachLayout() && ok;
  ok = TestAttachRejectsIncompatibleVersion() && ok;
  ok = TestTwoPhaseCommitVisibility() && ok;
  ok = TestReserveRejectsInvalidParameters() && ok;
  ok = TestInvalidStateTransitions() && ok;
  ok = TestAttachRejectsCorruptedHeader() && ok;
  ok = TestSequenceMonotonicAcrossSlots() && ok;
  ok = TestExplicitCommitAndReserveFailureSemantics() && ok;
  ok = TestConcurrentProducersReserveCommitStress() && ok;
  ok = TestVisibilityUnderProducerConsumerPressure() && ok;
  ok = TestBoundaryCapacityConditions() && ok;
  ok = TestBacklogWithSlowConsumerNoOverwrite() && ok;

  if (!ok) {
    LOG(ERROR) << "module3 ring buffer layout tests failed";
    mould::ShutdownApplicationLogging();
    return 1;
  }

  LOG(INFO) << "module3 ring buffer layout tests passed";
  mould::ShutdownApplicationLogging();
  return 0;
}
