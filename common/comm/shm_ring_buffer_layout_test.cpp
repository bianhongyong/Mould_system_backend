#include "ms_logging.hpp"
#include "shm_ring_buffer.hpp"
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <algorithm>
#include <iomanip>
#include <cstring>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using mould::comm::ComputeRingLayoutSizeBytes;
using mould::comm::ConsumerState;
using mould::comm::MessageEnvelope;
using mould::comm::ByteBuffer;
using mould::comm::RingLayoutView;
using mould::comm::RingSlotReservation;
using mould::comm::SlotState;

/// 消费者测试线程退出时必须下线，否则 `MinActiveReadSequence` 仍统计其游标，
/// `TryReclaimSlotIfSafe` 无法推进，生产者会永久卡在 `ReservePublishSlotRingOrdered`。
struct RingConsumerThreadOfflineGuard {
  RingLayoutView* ring = nullptr;
  std::uint32_t consumer_index = 0;
  RingConsumerThreadOfflineGuard(RingLayoutView* r, std::uint32_t idx) : ring(r), consumer_index(idx) {}
  ~RingConsumerThreadOfflineGuard() {
    if (ring != nullptr) {
      (void)ring->RemoveConsumerOnline(consumer_index);
    }
  }
  RingConsumerThreadOfflineGuard(const RingConsumerThreadOfflineGuard&) = delete;
  RingConsumerThreadOfflineGuard& operator=(const RingConsumerThreadOfflineGuard&) = delete;
};

bool PublishOneMessage(
    RingLayoutView* ring,
    std::uint32_t payload_size,
    const std::string& channel,
    std::uint64_t* out_sequence = nullptr) {
  if (ring == nullptr || payload_size == 0) {
    return false;
  }
  ByteBuffer payload(payload_size, 0);
  for (std::uint32_t i = 0; i < payload_size; ++i) {
    payload[i] = static_cast<std::uint8_t>((i % 251U) + 1U);
  }

  MessageEnvelope envelope{};
  if (!ring->PublishCommittedPayload(channel, std::move(payload), &envelope, nullptr, nullptr)) {
    return false;
  }
  if (out_sequence != nullptr) {
    *out_sequence = envelope.sequence;
  }
  return true;
}

bool ConsumeOneMessage(
    RingLayoutView* ring,
    std::uint32_t consumer_index,
    std::uint64_t* out_sequence = nullptr) {
  if (ring == nullptr) {
    return false;
  }
  constexpr int kMaxAttempts = 2048;
  RingSlotReservation committed{};
  std::uint32_t slot_index = 0;
  for (int i = 0; i < kMaxAttempts; ++i) {
    if (!ring->TryReadNextForConsumer(consumer_index, &committed, &slot_index)) {
      std::this_thread::yield();
      continue;
    }
    if (!ring->CompleteConsumerSlot(consumer_index, slot_index, committed.sequence + 1U)) {
      return false;
    }
    if (out_sequence != nullptr) {
      *out_sequence = committed.sequence;
    }
    return true;
  }
  return false;
}

TEST(RingBufferLayout, InitializeAndAttachLayout) {
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
  ASSERT_TRUE(owner.has_value()) << "initialize should succeed for valid ring layout";

  auto attached = RingLayoutView::Attach(backing.data(), backing.size());
  ASSERT_TRUE(attached.has_value()) << "attach should accept matching layout version";

  EXPECT_TRUE(attached->Header()->slot_count == kSlotCount &&
          attached->Header()->consumer_capacity == kConsumerCapacity)
      << "attached view should expose configured fixed layout";
}

TEST(RingBufferLayout, AttachRejectsIncompatibleVersion) {
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
  ASSERT_TRUE(owner.has_value()) << "initialize should succeed before mismatch attach";

  auto mismatch = RingLayoutView::Attach(backing.data(), backing.size(), owner->Header()->version + 1);
  EXPECT_TRUE(!mismatch.has_value()) << "attach should fail when ring layout version mismatches";
}

TEST(RingBufferLayout, TwoPhaseCommitVisibility) {
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
  ASSERT_TRUE(ring.has_value()) << "initialize should succeed for commit visibility test";

  RingSlotReservation reserved{};
  std::uint32_t reserved_slot = 0;
  ASSERT_TRUE(ring->ReservePublishSlotRingOrdered(32, &reserved, &reserved_slot) && reserved_slot == 0)
      << "ring-ordered reserve should succeed for first slot";

  RingSlotReservation read_before_commit{};
  ASSERT_TRUE(!ring->TryReadCommitted(0, &read_before_commit)) << "consumer should not read slot before commit";
  ASSERT_TRUE(static_cast<std::uint32_t>(ring->Slots()[0].state_sequence.load(std::memory_order_acquire) & 0xFFFFFFFFULL) ==
              static_cast<std::uint32_t>(SlotState::kReserved)) << "reserved slot must stay non-committed before publish";

  ASSERT_TRUE(ring->CommitSlot(0)) << "commit should transition slot to committed";

  RingSlotReservation read_after_commit{};
  ASSERT_TRUE(ring->TryReadCommitted(0, &read_after_commit)) << "consumer should read slot after commit";
  ASSERT_TRUE(read_after_commit.sequence == reserved.sequence &&
              read_after_commit.payload_size == reserved.payload_size) << "committed metadata should match reserved metadata";

  ASSERT_TRUE(ring->ResetSlot(0)) << "reset should release slot back to empty";
    EXPECT_TRUE(static_cast<std::uint32_t>(ring->Slots()[0].state_sequence.load(std::memory_order_acquire) & 0xFFFFFFFFULL) ==
      static_cast<std::uint32_t>(SlotState::kEmpty))
      << "reset slot should become empty";
}

TEST(RingBufferLayout, ReserveRejectsInvalidParameters) {
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
  ASSERT_TRUE(ring.has_value()) << "initialize should succeed for invalid parameter checks";

  RingSlotReservation out{};
  std::uint32_t slot_out = 0;
  ASSERT_TRUE(!ring->ReservePublishSlotRingOrdered(0, &out, &slot_out)) << "reserve should reject zero payload size";
  // 两槽均分 32 字节 payload，单槽容量 16；超过单槽容量的消息应被拒绝。
  ASSERT_TRUE(!ring->ReservePublishSlotRingOrdered(17, &out, &slot_out))
      << "reserve should reject payload larger than per-slot capacity";
  EXPECT_TRUE(!ring->ReservePublishSlotRingOrdered(8, &out, nullptr)) << "reserve should reject null out_slot_index pointer";
}

TEST(RingBufferLayout, InvalidStateTransitions) {
  // 单槽：第二条 ring-ordered 预留仍指向槽 0，可在未提交时验证「非 Empty 则预留失败」。
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
  ASSERT_TRUE(ring.has_value()) << "initialize should succeed for invalid transition checks";

  ASSERT_TRUE(!ring->CommitSlot(0)) << "commit should fail when slot is empty";

  RingSlotReservation out{};
  std::uint32_t slot_ix = 0;
  ASSERT_TRUE(ring->ReservePublishSlotRingOrdered(8, &out, &slot_ix) && slot_ix == 0) << "reserve should succeed for empty slot";
  RingSlotReservation dup{};
  std::uint32_t dup_slot = 0;
  ASSERT_TRUE(!ring->ReservePublishSlotRingOrdered(8, &dup, &dup_slot)) << "reserve should fail for non-empty slot";
  ASSERT_TRUE(ring->CommitSlot(0)) << "first commit should succeed";
  ASSERT_TRUE(!ring->CommitSlot(0)) << "second commit should fail for committed slot";
  ASSERT_TRUE(ring->ResetSlot(0)) << "reset should succeed after commit";
  EXPECT_TRUE(ring->ReservePublishSlotRingOrdered(8, &out, &slot_ix) && slot_ix == 0) << "slot should be reusable after reset";
}

TEST(RingBufferLayout, AttachRejectsCorruptedHeader) {
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
  ASSERT_TRUE(ring.has_value()) << "initialize should succeed before corruption checks";

  ring->Header()->magic = 0;
  ASSERT_TRUE(!RingLayoutView::Attach(backing.data(), backing.size()).has_value()) << "attach should reject mismatched magic";

  ring->Header()->magic = mould::comm::kRingLayoutMagic;
  ring->Header()->payload_region_bytes = static_cast<std::uint64_t>(backing.size());
  EXPECT_TRUE(!RingLayoutView::Attach(backing.data(), backing.size()).has_value())
      << "attach should reject incompatible header payload span";
}

TEST(RingBufferLayout, SequenceMonotonicAcrossSlots) {
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
  ASSERT_TRUE(ring.has_value()) << "initialize should succeed for sequence monotonic checks";

  RingSlotReservation first{};
  RingSlotReservation second{};
  std::uint32_t s0 = 0;
  std::uint32_t s1 = 0;
  ASSERT_TRUE(ring->ReservePublishSlotRingOrdered(16, &first, &s0) && s0 == 0) << "first ring-ordered reserve should succeed";
  ASSERT_TRUE(ring->ReservePublishSlotRingOrdered(16, &second, &s1) && s1 == 1) << "second ring-ordered reserve should succeed";
  EXPECT_TRUE(second.sequence > first.sequence) << "sequence should increase across different slot indexes";
}

TEST(RingBufferLayout, ExplicitCommitAndReserveFailureSemantics) {
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
  ASSERT_TRUE(ring.has_value()) << "initialize should succeed for explicit failure semantics";

  // 未 reserve 直接 commit 必须失败。
  ASSERT_TRUE(!ring->CommitSlot(0)) << "commit without reserve should fail";

  RingSlotReservation first{};
  std::uint32_t first_slot = 0;
  ASSERT_TRUE(ring->ReservePublishSlotRingOrdered(8, &first, &first_slot) && first_slot == 0) << "initial reserve should succeed";

  // 非 Empty 状态重复 reserve 必须失败。
  RingSlotReservation duplicate_reserve{};
  std::uint32_t dup_slot = 0;
  ASSERT_TRUE(!ring->ReservePublishSlotRingOrdered(8, &duplicate_reserve, &dup_slot)) << "reserve on non-empty slot should fail";

  ASSERT_TRUE(ring->CommitSlot(0)) << "first commit should succeed after reserve";

  // 重复 commit 必须失败。
  EXPECT_TRUE(!ring->CommitSlot(0)) << "repeated commit should fail";
}

TEST(RingBufferLayout, ConcurrentProducersReserveCommitStress) {
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
  ASSERT_TRUE(ring.has_value()) << "initialize should succeed for concurrent producer stress";

  const int total_messages = kThreads * kIterationsPerThread;
  std::vector<std::atomic<std::uint8_t>> seen(static_cast<std::size_t>(total_messages + 1));
  for (auto& flag : seen) {
    flag.store(0, std::memory_order_relaxed);
  }
  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&]() {
      for (int i = 0; i < kIterationsPerThread; ++i) {
        RingSlotReservation reservation{};
        std::uint32_t slot_index = 0;
        while (!ring->ReservePublishSlotRingOrdered(8, &reservation, &slot_index)) {
          std::this_thread::yield();
        }
        std::uint8_t* payload = ring->PayloadRegion();
        const std::uint64_t offset = reservation.payload_offset;
        const std::uint8_t marker = static_cast<std::uint8_t>((reservation.sequence % 251U) + 1U);
        for (int byte = 0; byte < 8; ++byte) {
          payload[offset + static_cast<std::uint64_t>(byte)] = marker;
        }
        if (!ring->CommitSlot(slot_index)) {
          failures.fetch_add(1, std::memory_order_relaxed);
          break;
        }

        RingSlotReservation committed{};
        if (!ring->TryReadCommitted(slot_index, &committed) || committed.sequence != reservation.sequence) {
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
        if (!ring->ResetSlot(slot_index)) {
          failures.fetch_add(1, std::memory_order_relaxed);
          break;
        }
      }
    });
  }

  for (auto& worker : workers) {
    worker.join();
  }

  ASSERT_TRUE(failures.load(std::memory_order_relaxed) == 0) << "concurrent producers should not observe state corruption";
  for (int seq = 1; seq <= total_messages; ++seq) {
    ASSERT_TRUE(seen[static_cast<std::size_t>(seq)].load(std::memory_order_acquire) == 1) << "all produced sequences should be unique and observed exactly once";
  }
}

TEST(RingBufferLayout, VisibilityUnderProducerConsumerPressure) {
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
  ASSERT_TRUE(ring.has_value()) << "initialize should succeed for visibility pressure";

  std::atomic<bool> producer_done{false};
  std::atomic<int> failures{0};
  std::atomic<std::uint64_t> last_sequence{0};

  std::thread producer([&]() {
    for (int i = 0; i < kIterations; ++i) {
      RingSlotReservation reservation{};
      std::uint32_t slot_index = 0;
      while (!ring->ReservePublishSlotRingOrdered(kMessageBytes, &reservation, &slot_index)) {
        std::this_thread::yield();
      }

      std::uint8_t* payload = ring->PayloadRegion();
      const std::uint8_t marker = static_cast<std::uint8_t>((reservation.sequence % 251U) + 1U);
      for (std::uint32_t b = 0; b < kMessageBytes; ++b) {
        payload[reservation.payload_offset + b] = marker;
      }

      if (!ring->CommitSlot(slot_index)) {
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

  EXPECT_TRUE(failures.load(std::memory_order_relaxed) == 0 &&
          last_sequence.load(std::memory_order_acquire) == static_cast<std::uint64_t>(kIterations))
      << "producer/consumer pressure should not observe torn payload or sequence regression";
}

TEST(RingBufferLayout, BoundaryCapacityConditions) {
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
  ASSERT_TRUE(!small_fail.has_value()) << "initialize should fail when total size is below exact minimum";

  std::vector<std::uint8_t> exact(small_layout + kSmallPayloadBytes, 0);
  auto small_ok = RingLayoutView::Initialize(
      exact.data(),
      exact.size(),
      kSmallSlotCount,
      kSmallConsumerCapacity,
      kSmallPayloadBytes);
  ASSERT_TRUE(small_ok.has_value()) << "initialize should succeed at exact minimum size";

  RingSlotReservation one_byte{};
  std::uint32_t small_slot = 0;
  ASSERT_TRUE(small_ok->ReservePublishSlotRingOrdered(1, &one_byte, &small_slot) && small_slot == 0)
      << "single-byte payload should fit in minimal payload region";
  ASSERT_TRUE(small_ok->CommitSlot(small_slot)) << "single-byte payload commit should succeed";

  constexpr std::uint32_t kLargeSlotCount = 16384;
  constexpr std::uint32_t kLargeConsumerCapacity = 2;
  // 每槽至少 1 字节，才能走 ring-ordered 预留；否则 per_slot_capacity 为 0。
  constexpr std::uint64_t kLargePayloadBytes = 16384;
  const std::size_t large_layout = ComputeRingLayoutSizeBytes(kLargeSlotCount, kLargeConsumerCapacity);
  std::vector<std::uint8_t> large(large_layout + kLargePayloadBytes, 0);
  auto large_ok = RingLayoutView::Initialize(
      large.data(),
      large.size(),
      kLargeSlotCount,
      kLargeConsumerCapacity,
      kLargePayloadBytes);
  ASSERT_TRUE(large_ok.has_value()) << "initialize should support high slot count within bounded memory";

  RingSlotReservation tail_slot{};
  std::uint32_t tail_slot_index = 0;
  EXPECT_TRUE(large_ok->ReservePublishSlotRingOrdered(1, &tail_slot, &tail_slot_index) && tail_slot_index == 0)
      << "ring-ordered reserve should succeed after initialize with large slot_count";
}

TEST(RingBufferLayout, BacklogWithSlowConsumerNoOverwrite) {
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
  ASSERT_TRUE(ring.has_value()) << "initialize should succeed for slow-consumer backlog test";
  ASSERT_TRUE(ring->AddConsumerOnline(0)) << "consumer 0 should register for ring-ordered read path";

  std::atomic<int> produced{0};
  std::atomic<int> consumed{0};
  std::atomic<int> failures{0};
  std::atomic<int> producer_reserve_failures{0};
  std::atomic<int> producer_max_consecutive_reserve_failures{0};

  std::thread producer([&]() {
    int local_produced = 0;
    int local_consecutive_reserve_failures = 0;
    int local_max_consecutive_reserve_failures = 0;
    while (local_produced < kIterations) {
      RingSlotReservation reservation{};
      std::uint32_t slot = 0;
      if (!ring->ReservePublishSlotRingOrdered(kMessageBytes, &reservation, &slot)) {
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
      const std::uint64_t payload_offset = reservation.payload_offset;
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
    RingConsumerThreadOfflineGuard offline_guard{&*ring, 0};
    int local_consumed = 0;
    while (local_consumed < kIterations) {
      RingSlotReservation committed{};
      std::uint32_t slot = 0;
      if (!ring->TryReadNextForConsumer(0, &committed, &slot)) {
        if (produced.load(std::memory_order_acquire) >= kIterations &&
            consumed.load(std::memory_order_acquire) >= kIterations) {
          break;
        }
        std::this_thread::yield();
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

      if (!ring->CompleteConsumerSlot(0, slot, committed.sequence + 1U)) {
        failures.fetch_add(1, std::memory_order_relaxed);
        return;
      }

      ++local_consumed;
      consumed.store(local_consumed, std::memory_order_release);

      // Deliberately slow down consumption to keep backlog pressure.
      std::this_thread::sleep_for(std::chrono::microseconds(40));
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

  EXPECT_TRUE(failures.load(std::memory_order_relaxed) == 0 &&
          produced.load(std::memory_order_acquire) == kIterations &&
          consumed.load(std::memory_order_acquire) == kIterations)
      << "slow-consumer backlog should not allow overwrite before consumption";
}

TEST(RingBufferLayout, ConsumerOwnerEpochAndForcedOffline) {
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
  ASSERT_TRUE(ring.has_value()) << "initialize should succeed for owner identity test";
  ASSERT_TRUE(ring->AddConsumerOnline(0)) << "consumer 0 should go online";
  const auto* consumers = ring->Consumers();
  ASSERT_TRUE(consumers != nullptr) << "consumer table should exist";
  const std::uint32_t pid = consumers[0].owner_pid;
  const std::uint32_t epoch = consumers[0].owner_start_epoch;
  ASSERT_TRUE(pid == static_cast<std::uint32_t>(getpid())) << "owner pid should match registering process";
  ASSERT_TRUE(ring->ConsumerOwnerMatches(0, pid, epoch)) << "owner tuple should match slot";
  ASSERT_TRUE(!ring->ConsumerOwnerMatches(0, pid, static_cast<std::uint32_t>(epoch + 1U))) << "wrong epoch must not match";
  ASSERT_TRUE(!ring->TakeConsumerOfflineIfOwner(0, pid, static_cast<std::uint32_t>(epoch + 1U))) << "wrong epoch should not offline";
  ASSERT_TRUE(ring->TakeConsumerOfflineIfOwner(0, pid, epoch)) << "matching tuple should offline consumer";
  ASSERT_TRUE(consumers[0].state.load(std::memory_order_acquire) ==
              static_cast<std::uint32_t>(ConsumerState::kOffline)) << "consumer should be offline after forced teardown";
  EXPECT_TRUE(consumers[0].owner_pid == 0 && consumers[0].owner_start_epoch == 0) << "owner metadata should clear on offline";
}

/// 多生产者并发发布 + 多消费者并发 `CompleteConsumerSlot`（同进程共享 `RingLayoutView`）。
/// 发布路径与 `PublishCommittedPayload` 一致：`ReservePublishSlotRingOrdered`（序号 S 固定槽 `(S-1)%N`）、
/// 写入 payload、`CommitSlot`；消费者用 `TryReadNextForConsumer` O(1) 取下一帧。
TEST(RingBufferLayout, MultiProducerMultiConsumerRingPublishStress) {
  constexpr std::uint32_t kSlotCount = 16;
  constexpr std::uint32_t kConsumerCapacity = 4;
  constexpr std::uint64_t kPayloadBytes = 4096;
  constexpr int kProducerThreads = 4;
  constexpr int kIterationsPerProducer = 200;
  constexpr int kConsumerThreads = static_cast<int>(kConsumerCapacity);
  constexpr std::uint32_t kMessageBytes = 32;
  constexpr int kTotalPublishes = kProducerThreads * kIterationsPerProducer;

  const std::size_t layout_bytes = ComputeRingLayoutSizeBytes(kSlotCount, kConsumerCapacity);
  std::vector<std::uint8_t> backing(layout_bytes + kPayloadBytes, 0);

  auto ring = RingLayoutView::Initialize(
      backing.data(),
      backing.size(),
      kSlotCount,
      kConsumerCapacity,
      kPayloadBytes);
  ASSERT_TRUE(ring.has_value()) << "initialize should succeed for MP+MC ring publish stress";

  auto* header = ring->Header();
  ASSERT_TRUE(header != nullptr) << "ring header should exist";

  for (std::uint32_t c = 0; c < kConsumerCapacity; ++c) {
    ASSERT_TRUE(ring->AddConsumerOnline(c)) << "consumer should register online";
  }

  std::atomic<bool> producers_done{false};
  std::atomic<int> publish_failures{0};
  std::atomic<int> consumer_failures{0};
  // (consumer_index, sequence) 各至多交付一次，验收多消费者 fan-out 下无重复/遗漏。
  const std::size_t delivery_stride = static_cast<std::size_t>(kTotalPublishes) + 2U;
  std::vector<std::atomic<std::uint8_t>> delivered(delivery_stride * static_cast<std::size_t>(kConsumerThreads));
  for (auto& cell : delivered) {
    cell.store(0, std::memory_order_relaxed);
  }

  std::vector<std::thread> producers;
  producers.reserve(static_cast<std::size_t>(kProducerThreads));
  for (int t = 0; t < kProducerThreads; ++t) {
    producers.emplace_back([&, t]() {
      for (int i = 0; i < kIterationsPerProducer; ++i) {
        const std::uint32_t slot_count = header->slot_count;
        const std::size_t payload_capacity = static_cast<std::size_t>(header->payload_region_bytes);
        const std::size_t per_slot_capacity =
            payload_capacity / static_cast<std::size_t>(std::max<std::uint32_t>(slot_count, 1U));
        if (per_slot_capacity < static_cast<std::size_t>(kMessageBytes)) {
          publish_failures.fetch_add(1, std::memory_order_relaxed);
          return;
        }

        RingSlotReservation reservation{};
        std::uint32_t slot_index = 0;
        while (!ring->ReservePublishSlotRingOrdered(kMessageBytes, &reservation, &slot_index)) {
          std::this_thread::yield();
        }

        std::uint8_t* payload_base = ring->PayloadRegion() + reservation.payload_offset;
        const std::uint8_t marker = static_cast<std::uint8_t>((reservation.sequence % 251U) + 1U);
        std::memset(payload_base, static_cast<int>(marker), static_cast<std::size_t>(kMessageBytes));
        payload_base[0] = static_cast<std::uint8_t>(t + 1);
        payload_base[1] = static_cast<std::uint8_t>(i % 255);

        if (!ring->CommitSlot(slot_index)) {
          publish_failures.fetch_add(1, std::memory_order_relaxed);
          return;
        }
      }
    });
  }

  std::vector<std::thread> consumers;
  consumers.reserve(static_cast<std::size_t>(kConsumerThreads));
  for (int c = 0; c < kConsumerThreads; ++c) {
    const std::uint32_t consumer_index = static_cast<std::uint32_t>(c);
    consumers.emplace_back([&, consumer_index]() {
      RingConsumerThreadOfflineGuard offline_guard{&*ring, consumer_index};
      while (true) {
        RingSlotReservation committed{};
        std::uint32_t slot = 0;
        if (!ring->TryReadNextForConsumer(consumer_index, &committed, &slot)) {
          if (producers_done.load(std::memory_order_acquire)) {
            if (publish_failures.load(std::memory_order_relaxed) > 0) {
              return;
            }
            // 已消费完最后一条后 read_sequence 变为 kTotalPublishes+1，下一帧 TryRead 恒失败；
            // 若仅依赖「读到 sequence==最后一条再 return」，竞态下可能再次进入循环而永远 yield，join 卡死。
            const auto done_cursor = ring->LoadConsumerCursor(consumer_index);
            if (done_cursor.has_value() &&
                *done_cursor > static_cast<std::uint64_t>(kTotalPublishes)) {
              return;
            }
          }
          std::this_thread::yield();
          continue;
        }
        if (committed.payload_size != kMessageBytes) {
          consumer_failures.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        const std::uint8_t* region = ring->PayloadRegion();
        const std::uint8_t expected_marker = static_cast<std::uint8_t>((committed.sequence % 251U) + 1U);
        for (std::uint32_t b = 2; b < committed.payload_size; ++b) {
          if (region[committed.payload_offset + b] != expected_marker) {
            consumer_failures.fetch_add(1, std::memory_order_relaxed);
            return;
          }
        }
        if (region[committed.payload_offset] == 0 || region[committed.payload_offset + 1] > 254) {
          consumer_failures.fetch_add(1, std::memory_order_relaxed);
          return;
        }

        if (!ring->CompleteConsumerSlot(consumer_index, slot, committed.sequence + 1)) {
          consumer_failures.fetch_add(1, std::memory_order_relaxed);
          return;
        }

        const std::size_t idx =
            static_cast<std::size_t>(consumer_index) * delivery_stride + static_cast<std::size_t>(committed.sequence);
        std::uint8_t expected_zero = 0;
        if (!delivered[idx].compare_exchange_strong(
                expected_zero,
                1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
          consumer_failures.fetch_add(1, std::memory_order_relaxed);
          return;
        }

        if (committed.sequence == static_cast<std::uint64_t>(kTotalPublishes)) {
          return;
        }
      }
    });
  }

  for (auto& th : producers) {
    th.join();
  }
  producers_done.store(true, std::memory_order_release);

  for (auto& th : consumers) {
    th.join();
  }

  ASSERT_TRUE(publish_failures.load(std::memory_order_relaxed) == 0) << "all concurrent publishes should succeed";
  ASSERT_TRUE(consumer_failures.load(std::memory_order_relaxed) == 0) << "consumers should not observe torn or duplicate delivery";

  for (int c = 0; c < kConsumerThreads; ++c) {
    for (int s = 1; s <= kTotalPublishes; ++s) {
      const std::size_t idx = static_cast<std::size_t>(c) * delivery_stride + static_cast<std::size_t>(s);
      ASSERT_TRUE(delivered[idx].load(std::memory_order_acquire) == 1) << "each consumer should observe each sequence exactly once under MP+MC contention";
    }
  }

  const std::uint64_t next_seq = ring->Header()->next_sequence.load(std::memory_order_acquire);
  EXPECT_TRUE(next_seq == static_cast<std::uint64_t>(kTotalPublishes) + 1U)
      << "global sequence should advance exactly once per successful publish";
}

/// 单通道 `RingLayoutView` 上多生产者 / 多消费者 fan-out：总发布条数固定为 6000，
/// 在不同 (生产者线程数, 消费者线程数) 组合下使用 `ReservePublishSlotRingOrdered` + `TryReadNextForConsumer`，
/// 统计墙钟时间并输出发布吞吐与 fan-out 总交付吞吐。
TEST(RingBufferLayout, MultiProducerMultiConsumerRingPublishThroughputVariedConfigs6000) {
  // 可调大（如 60000）做本地压测；CI 默认保持较小总量以控制耗时。
  constexpr int kTotalMessages = 600000;
  constexpr std::uint32_t kSlotCount = 32;
  constexpr std::uint32_t kConsumerCapacity = 8;
  constexpr std::uint64_t kPayloadBytes = 4096;
  constexpr std::uint32_t kMessageBytes = 32;

  struct Scenario {
    int producer_threads;
    int consumer_threads;
  };
  const Scenario scenarios[] = {
      {1, 1},
      {2, 2},
      {3, 4},
      {4, 4},
      {6, 3},
      {4, 6},
      {8, 2},
      {2, 8},
      {6, 6},
  };

  for (const Scenario& sc : scenarios) {
    if (sc.producer_threads <= 0 || sc.consumer_threads <= 0) {
      continue;
    }
    ASSERT_LE(sc.consumer_threads, static_cast<int>(kConsumerCapacity))
        << "scenario consumer_threads must not exceed ring consumer_capacity";

    std::vector<int> publishes_per_producer(static_cast<std::size_t>(sc.producer_threads));
    {
      const int base = kTotalMessages / sc.producer_threads;
      const int rem = kTotalMessages % sc.producer_threads;
      for (int t = 0; t < sc.producer_threads; ++t) {
        publishes_per_producer[static_cast<std::size_t>(t)] = base + (t < rem ? 1 : 0);
      }
    }

    const std::size_t layout_bytes = ComputeRingLayoutSizeBytes(kSlotCount, kConsumerCapacity);
    std::vector<std::uint8_t> backing(layout_bytes + kPayloadBytes, 0);

    auto ring = RingLayoutView::Initialize(
        backing.data(),
        backing.size(),
        kSlotCount,
        kConsumerCapacity,
        kPayloadBytes);
    ASSERT_TRUE(ring.has_value()) << "initialize should succeed for MP+MC throughput scenario";

    auto* header = ring->Header();
    ASSERT_TRUE(header != nullptr) << "ring header should exist";

    for (int c = 0; c < sc.consumer_threads; ++c) {
      ASSERT_TRUE(ring->AddConsumerOnline(static_cast<std::uint32_t>(c))) << "consumer should register online";
    }

    std::atomic<bool> producers_done{false};
    std::atomic<int> publish_failures{0};
    std::atomic<int> consumer_failures{0};
    const std::size_t delivery_stride = static_cast<std::size_t>(kTotalMessages) + 2U;
    std::vector<std::atomic<std::uint8_t>> delivered(
        delivery_stride * static_cast<std::size_t>(sc.consumer_threads));
    for (auto& cell : delivered) {
      cell.store(0, std::memory_order_relaxed);
    }
    std::vector<std::atomic<std::int64_t>> consumer_received_counts(
        static_cast<std::size_t>(sc.consumer_threads));
    for (auto& cnt : consumer_received_counts) {
      cnt.store(0, std::memory_order_relaxed);
    }

    const auto bench_begin = std::chrono::steady_clock::now();

    std::vector<std::thread> producers;
    producers.reserve(static_cast<std::size_t>(sc.producer_threads));
    for (int t = 0; t < sc.producer_threads; ++t) {
      const int iterations = publishes_per_producer[static_cast<std::size_t>(t)];
      producers.emplace_back([&, t, iterations]() {
        for (int i = 0; i < iterations; ++i) {
          const std::uint32_t slot_count = header->slot_count;
          const std::size_t payload_capacity = static_cast<std::size_t>(header->payload_region_bytes);
          const std::size_t per_slot_capacity =
              payload_capacity / static_cast<std::size_t>(std::max<std::uint32_t>(slot_count, 1U));
          if (per_slot_capacity < static_cast<std::size_t>(kMessageBytes)) {
            publish_failures.fetch_add(1, std::memory_order_relaxed);
            return;
          }

          RingSlotReservation reservation{};
          std::uint32_t slot_index = 0;
          while (!ring->ReservePublishSlotRingOrdered(kMessageBytes, &reservation, &slot_index)) {
            std::this_thread::yield();
          }

          std::uint8_t* payload_base = ring->PayloadRegion() + reservation.payload_offset;
          const std::uint8_t marker = static_cast<std::uint8_t>((reservation.sequence % 251U) + 1U);
          std::memset(payload_base, static_cast<int>(marker), static_cast<std::size_t>(kMessageBytes));
          payload_base[0] = static_cast<std::uint8_t>(t + 1);
          payload_base[1] = static_cast<std::uint8_t>(i % 255);

          if (!ring->CommitSlot(slot_index)) {
            publish_failures.fetch_add(1, std::memory_order_relaxed);
            return;
          }
        }
      });
    }

    std::vector<std::thread> consumers;
    consumers.reserve(static_cast<std::size_t>(sc.consumer_threads));
    for (int c = 0; c < sc.consumer_threads; ++c) {
      const std::uint32_t consumer_index = static_cast<std::uint32_t>(c);
      consumers.emplace_back([&, consumer_index]() {
        RingConsumerThreadOfflineGuard offline_guard{&*ring, consumer_index};
        while (true) {
          RingSlotReservation committed{};
          std::uint32_t slot = 0;
          if (!ring->TryReadNextForConsumer(consumer_index, &committed, &slot)) {
            if (producers_done.load(std::memory_order_acquire)) {
              if (publish_failures.load(std::memory_order_relaxed) > 0) {
                return;
              }
              const auto done_cursor = ring->LoadConsumerCursor(consumer_index);
              if (done_cursor.has_value() &&
                  *done_cursor > static_cast<std::uint64_t>(kTotalMessages)) {
                return;
              }
            }
            std::this_thread::yield();
            continue;
          }
          if (committed.payload_size != kMessageBytes) {
            consumer_failures.fetch_add(1, std::memory_order_relaxed);
            return;
          }
          const std::uint8_t* region = ring->PayloadRegion();
          const std::uint8_t expected_marker = static_cast<std::uint8_t>((committed.sequence % 251U) + 1U);
          for (std::uint32_t b = 2; b < committed.payload_size; ++b) {
            if (region[committed.payload_offset + b] != expected_marker) {
              consumer_failures.fetch_add(1, std::memory_order_relaxed);
              return;
            }
          }
          if (region[committed.payload_offset] == 0 || region[committed.payload_offset + 1] > 254) {
            consumer_failures.fetch_add(1, std::memory_order_relaxed);
            return;
          }

          if (!ring->CompleteConsumerSlot(consumer_index, slot, committed.sequence + 1)) {
            consumer_failures.fetch_add(1, std::memory_order_relaxed);
            return;
          }

          const std::size_t idx =
              static_cast<std::size_t>(consumer_index) * delivery_stride + static_cast<std::size_t>(committed.sequence);
          std::uint8_t expected_zero = 0;
          if (!delivered[idx].compare_exchange_strong(
                  expected_zero,
                  1,
                  std::memory_order_acq_rel,
                  std::memory_order_acquire)) {
            consumer_failures.fetch_add(1, std::memory_order_relaxed);
            return;
          }
          consumer_received_counts[static_cast<std::size_t>(consumer_index)]
              .fetch_add(1, std::memory_order_relaxed);

          if (committed.sequence == static_cast<std::uint64_t>(kTotalMessages)) {
            return;
          }
        }
      });
    }

    for (auto& th : producers) {
      th.join();
    }
    producers_done.store(true, std::memory_order_release);

    for (auto& th : consumers) {
      th.join();
    }

    const auto bench_end = std::chrono::steady_clock::now();
    const double wall_sec =
        std::chrono::duration<double>(bench_end - bench_begin).count();
    const double wall_ms = wall_sec * 1000.0;
    const double produce_msgs_per_s = wall_sec > 0.0 ? static_cast<double>(kTotalMessages) / wall_sec : 0.0;
    const std::int64_t total_fanout_deliveries =
        static_cast<std::int64_t>(kTotalMessages) * static_cast<std::int64_t>(sc.consumer_threads);
    const double fanout_deliveries_per_s =
        wall_sec > 0.0 ? static_cast<double>(total_fanout_deliveries) / wall_sec : 0.0;

    LOG(INFO) << std::fixed << std::setprecision(3) << "[mp-mc-throughput-6000] producers="
              << sc.producer_threads << " consumers=" << sc.consumer_threads << " total_publishes=" << kTotalMessages
              << " wall_ms=" << wall_ms << " publish_msgs_per_s=" << produce_msgs_per_s
              << " fanout_complete_consumer_slot_per_s=" << fanout_deliveries_per_s;

    for (int c = 0; c < sc.consumer_threads; ++c) {
      const std::int64_t received =
        consumer_received_counts[static_cast<std::size_t>(c)].load(std::memory_order_acquire);
      LOG(INFO) << "[mp-mc-throughput-6000-consumer-count] producers="
          << sc.producer_threads << " consumers=" << sc.consumer_threads
          << " consumer_index=" << c << " received=" << received
          << " expected=" << kTotalMessages;
      ASSERT_EQ(received, static_cast<std::int64_t>(kTotalMessages))
        << "each consumer must receive exactly kTotalMessages";
    }

    ASSERT_TRUE(publish_failures.load(std::memory_order_relaxed) == 0) << "all publishes should succeed in throughput scenario";
    ASSERT_TRUE(consumer_failures.load(std::memory_order_relaxed) == 0) << "consumers should not observe errors in throughput scenario";

    for (int c = 0; c < sc.consumer_threads; ++c) {
      for (int s = 1; s <= kTotalMessages; ++s) {
        const std::size_t idx = static_cast<std::size_t>(c) * delivery_stride + static_cast<std::size_t>(s);
        ASSERT_TRUE(delivered[idx].load(std::memory_order_acquire) == 1) << "each consumer should observe each sequence exactly once";
      }
    }

    const std::uint64_t next_seq = ring->Header()->next_sequence.load(std::memory_order_acquire);
    ASSERT_TRUE(next_seq == static_cast<std::uint64_t>(kTotalMessages) + 1U) << "global sequence should match total publishes";
  }

}

/// 环形发布：`ReservePublishSlotRingOrdered` 将 S 绑定到槽 `(S-1)%N`；`TryReadNextForConsumer` O(1) 与之一致。
TEST(RingBufferLayout, RingOrderedReserveAndTryReadNextForConsumerO1) {
  constexpr std::uint32_t kSlotCount = 4;
  constexpr std::uint32_t kConsumerCapacity = 1;
  constexpr std::uint64_t kPayloadBytes = 512;
  const std::size_t layout_bytes = ComputeRingLayoutSizeBytes(kSlotCount, kConsumerCapacity);
  std::vector<std::uint8_t> backing(layout_bytes + kPayloadBytes, 0);

  auto ring = RingLayoutView::Initialize(
      backing.data(),
      backing.size(),
      kSlotCount,
      kConsumerCapacity,
      kPayloadBytes);
  ASSERT_TRUE(ring.has_value()) << "initialize should succeed for ring-ordered publish test";
  ASSERT_TRUE(ring->AddConsumerOnline(0)) << "consumer should register online";

  RingSlotReservation r1{};
  std::uint32_t s1 = 999;
  ASSERT_TRUE(ring->ReservePublishSlotRingOrdered(8, &r1, &s1)) << "first ring-ordered reserve should succeed";
  ASSERT_TRUE(r1.sequence == 1U && s1 == 0U) << "sequence 1 should map to slot (1-1)%4 == 0";
  ASSERT_TRUE(ring->CommitSlot(s1)) << "commit first message";

  RingSlotReservation r2{};
  std::uint32_t s2 = 999;
  ASSERT_TRUE(ring->ReservePublishSlotRingOrdered(8, &r2, &s2)) << "second ring-ordered reserve should succeed";
  ASSERT_TRUE(r2.sequence == 2U && s2 == 1U) << "sequence 2 should map to slot (2-1)%4 == 1";
  ASSERT_TRUE(ring->CommitSlot(s2)) << "commit second message";

  RingSlotReservation read{};
  std::uint32_t slot_index = 999;
  ASSERT_TRUE(ring->TryReadNextForConsumer(0, &read, &slot_index)) << "TryReadNextForConsumer should read seq 1";
  ASSERT_TRUE(read.sequence == 1U && slot_index == 0U) << "O(1) read should hit slot 0 for read_sequence==1";
  ASSERT_TRUE(ring->CompleteConsumerSlot(0, slot_index, read.sequence + 1U)) << "complete first delivery";

  ASSERT_TRUE(ring->TryReadNextForConsumer(0, &read, &slot_index)) << "TryReadNextForConsumer should read seq 2";
  ASSERT_TRUE(read.sequence == 2U && slot_index == 1U) << "O(1) read should hit slot 1 for read_sequence==2";
  EXPECT_TRUE(ring->CompleteConsumerSlot(0, slot_index, read.sequence + 1U)) << "complete second delivery";
}

TEST(RingBufferLayout, SingleProducerDynamicMembershipPartialAndAllOfflineWindows) {
  constexpr std::uint32_t kSlotCount = 16;
  constexpr std::uint32_t kConsumerCapacity = 4;
  constexpr std::uint64_t kPayloadBytes = 4096;
  constexpr std::uint32_t kMessageBytes = 32;

  const std::size_t layout_bytes = ComputeRingLayoutSizeBytes(kSlotCount, kConsumerCapacity);
  std::vector<std::uint8_t> backing(layout_bytes + kPayloadBytes, 0);
  auto ring = RingLayoutView::Initialize(
      backing.data(),
      backing.size(),
      kSlotCount,
      kConsumerCapacity,
      kPayloadBytes);
  ASSERT_TRUE(ring.has_value()) << "initialize should succeed for dynamic membership test";

  ASSERT_TRUE(ring->AddConsumerOnline(0)) << "consumer 0 should be online";
  ASSERT_TRUE(ring->AddConsumerOnline(1)) << "consumer 1 should be online";
  ASSERT_TRUE(ring->AddConsumerOnline(2)) << "consumer 2 should be online";

  ASSERT_TRUE(ring->RemoveConsumerOnline(1)) << "consumer 1 should go offline (partial offline window)";
  ASSERT_TRUE(ring->RemoveConsumerOnline(2)) << "consumer 2 should go offline (partial offline window)";

  for (int i = 0; i < 80; ++i) {
    std::uint64_t published_sequence = 0;
    std::uint64_t consumed_sequence = 0;
    ASSERT_TRUE(PublishOneMessage(&(*ring), kMessageBytes, "partial-window", &published_sequence))
        << "single producer should publish while subset consumers are offline";
    ASSERT_TRUE(ConsumeOneMessage(&(*ring), 0, &consumed_sequence))
        << "remaining online consumer should continue draining";
    ASSERT_EQ(consumed_sequence, published_sequence)
        << "online consumer should observe newly published sequence";
  }

  ASSERT_TRUE(ring->AddConsumerOnline(1)) << "consumer 1 should rejoin";
  const auto c1_cursor = ring->LoadConsumerCursor(1);
  ASSERT_TRUE(c1_cursor.has_value()) << "rejoined consumer should have cursor";
  EXPECT_EQ(*c1_cursor, ring->Header()->next_sequence.load(std::memory_order_seq_cst))
      << "rejoined consumer starts from current ring head and does not replay offline backlog";

  ASSERT_TRUE(ring->RemoveConsumerOnline(0)) << "consumer 0 should go offline";
  ASSERT_TRUE(ring->RemoveConsumerOnline(1)) << "consumer 1 should go offline";
  const std::uint64_t gap_begin = ring->Header()->next_sequence.load(std::memory_order_seq_cst);

  for (int i = 0; i < 256; ++i) {
    ASSERT_TRUE(PublishOneMessage(&(*ring), kMessageBytes, "all-offline-gap", nullptr))
        << "producer should keep publishing when all consumers are offline";
  }
  const std::uint64_t gap_end = ring->Header()->next_sequence.load(std::memory_order_seq_cst);
  EXPECT_GT(gap_end, gap_begin) << "sequence should continue advancing through all-offline window";

  ASSERT_TRUE(ring->AddConsumerOnline(2)) << "consumer 2 should come online after all-offline window";
  const auto c2_cursor = ring->LoadConsumerCursor(2);
  ASSERT_TRUE(c2_cursor.has_value()) << "consumer 2 should have cursor after rejoin";
  EXPECT_EQ(*c2_cursor, gap_end)
      << "messages produced during all-offline window are allowed to be dropped";

  std::uint64_t published_after_rejoin = 0;
  std::uint64_t consumed_after_rejoin = 0;
  ASSERT_TRUE(PublishOneMessage(&(*ring), kMessageBytes, "after-rejoin", &published_after_rejoin))
      << "producer should publish after consumer rejoin";
  ASSERT_TRUE(ConsumeOneMessage(&(*ring), 2, &consumed_after_rejoin))
      << "rejoined consumer should consume new messages";
  EXPECT_EQ(consumed_after_rejoin, published_after_rejoin)
      << "rejoined consumer should read newly produced sequence";
  EXPECT_GE(consumed_after_rejoin, gap_end)
      << "first consumed sequence after rejoin should be at or after rejoin head";
}

TEST(RingBufferLayout, SingleProducerAllOfflineThenPartialOnlineFanoutResumes) {
  constexpr std::uint32_t kSlotCount = 8;
  constexpr std::uint32_t kConsumerCapacity = 4;
  constexpr std::uint64_t kPayloadBytes = 2048;
  constexpr std::uint32_t kMessageBytes = 32;

  const std::size_t layout_bytes = ComputeRingLayoutSizeBytes(kSlotCount, kConsumerCapacity);
  std::vector<std::uint8_t> backing(layout_bytes + kPayloadBytes, 0);
  auto ring = RingLayoutView::Initialize(
      backing.data(),
      backing.size(),
      kSlotCount,
      kConsumerCapacity,
      kPayloadBytes);
  ASSERT_TRUE(ring.has_value()) << "initialize should succeed for all-offline/partial-online test";

  ASSERT_TRUE(ring->AddConsumerOnline(0)) << "consumer 0 should be online";
  ASSERT_TRUE(ring->AddConsumerOnline(1)) << "consumer 1 should be online";
  ASSERT_TRUE(ring->AddConsumerOnline(2)) << "consumer 2 should be online";
  ASSERT_TRUE(ring->AddConsumerOnline(3)) << "consumer 3 should be online";

  for (int i = 0; i < 40; ++i) {
    std::uint64_t published_sequence = 0;
    std::uint64_t consumed0 = 0;
    std::uint64_t consumed1 = 0;
    std::uint64_t consumed2 = 0;
    std::uint64_t consumed3 = 0;
    ASSERT_TRUE(PublishOneMessage(&(*ring), kMessageBytes, "warmup", &published_sequence));
    ASSERT_TRUE(ConsumeOneMessage(&(*ring), 0, &consumed0));
    ASSERT_TRUE(ConsumeOneMessage(&(*ring), 1, &consumed1));
    ASSERT_TRUE(ConsumeOneMessage(&(*ring), 2, &consumed2));
    ASSERT_TRUE(ConsumeOneMessage(&(*ring), 3, &consumed3));
    ASSERT_EQ(consumed0, published_sequence);
    ASSERT_EQ(consumed1, published_sequence);
    ASSERT_EQ(consumed2, published_sequence);
    ASSERT_EQ(consumed3, published_sequence);
  }

  ASSERT_TRUE(ring->RemoveConsumerOnline(0)) << "consumer 0 should go offline";
  ASSERT_TRUE(ring->RemoveConsumerOnline(1)) << "consumer 1 should go offline";
  ASSERT_TRUE(ring->RemoveConsumerOnline(2)) << "consumer 2 should go offline";
  ASSERT_TRUE(ring->RemoveConsumerOnline(3)) << "consumer 3 should go offline";

  const std::uint64_t gap_begin = ring->Header()->next_sequence.load(std::memory_order_seq_cst);
  for (int i = 0; i < 200; ++i) {
    ASSERT_TRUE(PublishOneMessage(&(*ring), kMessageBytes, "all-offline", nullptr))
        << "single producer should keep running with zero online consumers";
  }
  const std::uint64_t gap_end = ring->Header()->next_sequence.load(std::memory_order_seq_cst);
  EXPECT_GT(gap_end, gap_begin) << "sequence should move forward in all-offline gap";

  ASSERT_TRUE(ring->AddConsumerOnline(1)) << "consumer 1 should rejoin";
  ASSERT_TRUE(ring->AddConsumerOnline(3)) << "consumer 3 should rejoin";

  const auto c1_cursor = ring->LoadConsumerCursor(1);
  const auto c3_cursor = ring->LoadConsumerCursor(3);
  ASSERT_TRUE(c1_cursor.has_value() && c3_cursor.has_value())
      << "rejoined consumers should expose online cursors";
  EXPECT_EQ(*c1_cursor, gap_end);
  EXPECT_EQ(*c3_cursor, gap_end);

  for (int i = 0; i < 24; ++i) {
    std::uint64_t published_sequence = 0;
    std::uint64_t consumed1 = 0;
    std::uint64_t consumed3 = 0;
    ASSERT_TRUE(PublishOneMessage(&(*ring), kMessageBytes, "fanout-resume", &published_sequence));
    ASSERT_TRUE(ConsumeOneMessage(&(*ring), 1, &consumed1));
    ASSERT_TRUE(ConsumeOneMessage(&(*ring), 3, &consumed3));
    EXPECT_EQ(consumed1, published_sequence) << "consumer 1 should resume fanout delivery";
    EXPECT_EQ(consumed3, published_sequence) << "consumer 3 should resume fanout delivery";
    EXPECT_GE(consumed1, gap_end) << "post-rejoin data should be at/after rejoin head";
    EXPECT_GE(consumed3, gap_end) << "post-rejoin data should be at/after rejoin head";
  }
}

}  // namespace

int main(int argc, char** argv) {
  mould::InitApplicationLogging(argv[0]);
  ::testing::InitGoogleTest(&argc, argv);
  const int result = RUN_ALL_TESTS();
  mould::ShutdownApplicationLogging();
  return result;
}
