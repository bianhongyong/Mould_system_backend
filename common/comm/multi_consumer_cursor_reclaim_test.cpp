#include "ms_logging.hpp"
#include "channel_topology_config.hpp"
#include "shm_bus_runtime.hpp"
#include "shm_ring_buffer.hpp"
#include "shm_segment.hpp"
#include "test_helpers.hpp"

#include <gtest/gtest.h>

#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class MultiConsumerCursorReclaimGlogEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    mould::InitApplicationLogging("multi_consumer_cursor_reclaim_test");
  }
  void TearDown() override { mould::ShutdownApplicationLogging(); }
};

const auto* const k_multi_consumer_cursor_reclaim_glog_env =
    ::testing::AddGlobalTestEnvironment(new MultiConsumerCursorReclaimGlogEnvironment());

namespace {

using mould::comm::BuildDeterministicShmName;
using mould::comm::ComputeRingLayoutSizeBytes;
using mould::comm::MessageEnvelope;
using mould::comm::RingLayoutView;
using mould::comm::RingSlotReservation;
using mould::comm::ShmSegment;
using mould::comm::ShmSegmentLayout;
using mould::comm::ShmBusRuntime;

constexpr std::uint32_t kSlotCount = 8;
constexpr std::uint32_t kConsumerCapacity = 4;
constexpr std::uint64_t kPayloadBytes = 512;

std::string BuildUniqueChannelName(const std::string& suffix) {
  return "module4_mp_" + suffix + "_" + std::to_string(::getpid()) + "_" +
      std::to_string(
             static_cast<std::uint64_t>(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
}

bool WaitForCondition(const std::function<bool()>& predicate, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
}

std::optional<RingLayoutView> AttachRing(ShmSegment* segment) {
  if (segment == nullptr) {
    return std::nullopt;
  }
  auto* ring_base = static_cast<std::uint8_t*>(segment->BaseAddress()) + mould::comm::ShmSegmentHeaderSizeBytes();
  const std::size_t ring_span = segment->SizeBytes() - mould::comm::ShmSegmentHeaderSizeBytes();
  return RingLayoutView::Attach(ring_base, ring_span);
}

bool TestConsumerCursorRegisterAdvanceAndPersistCrossProcess() {
  const std::string channel = BuildUniqueChannelName("cursor");
  const std::string shm_name = BuildDeterministicShmName(channel);
  shm_unlink(shm_name.c_str());

  ShmSegmentLayout layout;
  layout.payload_capacity = ComputeRingLayoutSizeBytes(kSlotCount, kConsumerCapacity) + kPayloadBytes;
  auto segment = ShmSegment::CreateOrAttach(channel, layout);
  if (!Check(segment.has_value(), "parent should create shm segment")) {
    return false;
  }
  auto ring = RingLayoutView::Initialize(
      static_cast<std::uint8_t*>(segment->BaseAddress()) + mould::comm::ShmSegmentHeaderSizeBytes(),
      segment->SizeBytes() - mould::comm::ShmSegmentHeaderSizeBytes(),
      kSlotCount,
      kConsumerCapacity,
      kPayloadBytes);
  if (!Check(ring.has_value(), "parent should initialize ring layout")) {
    shm_unlink(shm_name.c_str());
    return false;
  }

  pid_t pid = fork();
  if (!Check(pid >= 0, "fork should succeed")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (pid == 0) {
    auto child_segment = ShmSegment::CreateOrAttach(channel, layout);
    auto child_ring = child_segment ? AttachRing(&(*child_segment)) : std::nullopt;
    const bool ok = child_ring.has_value() && child_ring->RegisterConsumer(1, 3) &&
        child_ring->AdvanceConsumerCursor(1, 5) && !child_ring->AdvanceConsumerCursor(1, 4);
    _exit(ok ? 0 : 10);
  }

  if (!Check(ring->RegisterConsumer(0, 1), "register consumer-0 should succeed")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (!Check(!ring->RegisterConsumer(kConsumerCapacity, 1), "register out-of-range consumer should fail")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (!Check(!ring->AdvanceConsumerCursor(2, 5), "advance on inactive consumer should fail")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (!Check(ring->AdvanceConsumerCursor(0, 6), "advance should move cursor forward")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (!Check(!ring->AdvanceConsumerCursor(0, 2), "advance should reject regression")) {
    shm_unlink(shm_name.c_str());
    return false;
  }

  int status = 0;
  if (!Check(waitpid(pid, &status, 0) == pid, "waitpid should succeed")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (!Check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child process checks should pass")) {
    shm_unlink(shm_name.c_str());
    return false;
  }

  const auto cursor0 = ring->LoadConsumerCursor(0);
  const auto cursor1 = ring->LoadConsumerCursor(1);
  const bool ok = Check(
                      cursor0.has_value() && *cursor0 == 6 && cursor1.has_value() && *cursor1 == 5,
                      "consumer cursors should persist in shared table") &&
      Check(ring->UnregisterConsumer(1), "unregister consumer-1 should succeed") &&
      Check(!ring->LoadConsumerCursor(1).has_value(), "inactive consumer cursor should not be loadable");
  shm_unlink(shm_name.c_str());
  return ok;
}

bool TestReclaimUsesMinActiveCursorCrossProcess() {
  const std::string channel = BuildUniqueChannelName("reclaim");
  const std::string shm_name = BuildDeterministicShmName(channel);
  shm_unlink(shm_name.c_str());

  ShmSegmentLayout layout;
  layout.payload_capacity = ComputeRingLayoutSizeBytes(4, 2) + 256;
  auto segment = ShmSegment::CreateOrAttach(channel, layout);
  auto ring = segment
      ? RingLayoutView::Initialize(
            static_cast<std::uint8_t*>(segment->BaseAddress()) + mould::comm::ShmSegmentHeaderSizeBytes(),
            segment->SizeBytes() - mould::comm::ShmSegmentHeaderSizeBytes(),
            4,
            2,
            256)
                      : std::nullopt;
  if (!Check(ring.has_value(), "parent should initialize ring for reclaim test")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (!Check(ring->RegisterConsumer(0, 1), "register fast consumer should succeed")) {
    shm_unlink(shm_name.c_str());
    return false;
  }

  pid_t pid = fork();
  if (!Check(pid >= 0, "fork should succeed")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (pid == 0) {
    auto child_segment = ShmSegment::CreateOrAttach(channel, layout);
    auto child_ring = child_segment ? AttachRing(&(*child_segment)) : std::nullopt;
    bool ok = child_ring.has_value() && child_ring->RegisterConsumer(1, 1) &&
        child_ring->AdvanceConsumerCursor(1, 2);
    if (ok) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      ok = child_ring->AdvanceConsumerCursor(1, 4);
    }
    _exit(ok ? 0 : 11);
  }

  for (std::uint32_t i = 0; i < 3; ++i) {
    RingSlotReservation reservation{};
    std::uint32_t slot_index = 0;
    if (!Check(
            ring->ReservePublishSlotRingOrdered(8, &reservation, &slot_index) && slot_index == i,
            "ring-ordered reserve should map sequence to expected slot before reclaim")) {
      shm_unlink(shm_name.c_str());
      return false;
    }
    if (!Check(ring->CommitSlot(slot_index), "commit should succeed before reclaim")) {
      shm_unlink(shm_name.c_str());
      return false;
    }
  }
  if (!Check(ring->AdvanceConsumerCursor(0, 4), "fast consumer advances further")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (!Check(
          WaitForCondition(
              [&]() {
                const auto cursor = ring->LoadConsumerCursor(1);
                return cursor.has_value() && *cursor >= 2;
              },
              500),
          "slow consumer cursor should reach first step in child process")) {
    shm_unlink(shm_name.c_str());
    return false;
  }

  const std::uint64_t first_reclaim = ring->ReclaimCommittedSlots();
  if (!Check(first_reclaim == 1, "reclaim should only release slots below min active cursor")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (!Check(ring->Header()->reclaim_sequence.load() == 1, "reclaim watermark should track reclaimed sequence")) {
    shm_unlink(shm_name.c_str());
    return false;
  }

  if (!Check(
          WaitForCondition(
              [&]() {
                const auto cursor = ring->LoadConsumerCursor(1);
                return cursor.has_value() && *cursor >= 4;
              },
              500),
          "slow consumer should eventually catch up in child process")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  const std::uint64_t second_reclaim = ring->ReclaimCommittedSlots();
  if (!Check(second_reclaim == 2, "reclaim should release remaining committed slots after catch-up")) {
    shm_unlink(shm_name.c_str());
    return false;
  }

  int status = 0;
  const bool child_ok = waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  shm_unlink(shm_name.c_str());
  return Check(child_ok, "child process reclaim-side cursor flow should pass");
}

bool TestSlowConsumerLagAndIsolationCrossProcess() {
  const std::string channel = BuildUniqueChannelName("lag");
  const std::string shm_name = BuildDeterministicShmName(channel);
  shm_unlink(shm_name.c_str());

  ShmSegmentLayout layout;
  layout.payload_capacity = ComputeRingLayoutSizeBytes(4, 2) + 256;
  auto segment = ShmSegment::CreateOrAttach(channel, layout);
  auto ring = segment
      ? RingLayoutView::Initialize(
            static_cast<std::uint8_t*>(segment->BaseAddress()) + mould::comm::ShmSegmentHeaderSizeBytes(),
            segment->SizeBytes() - mould::comm::ShmSegmentHeaderSizeBytes(),
            4,
            2,
            256)
                      : std::nullopt;
  if (!Check(ring.has_value(), "parent should initialize ring for lag test")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (!Check(ring->RegisterConsumer(0, 1), "register consumer-0 should succeed")) {
    shm_unlink(shm_name.c_str());
    return false;
  }

  pid_t pid = fork();
  if (!Check(pid >= 0, "fork should succeed")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (pid == 0) {
    auto child_segment = ShmSegment::CreateOrAttach(channel, layout);
    auto child_ring = child_segment ? AttachRing(&(*child_segment)) : std::nullopt;
    const bool ok = child_ring.has_value() && child_ring->RegisterConsumer(1, 1) &&
        child_ring->AdvanceConsumerCursor(1, 2);
    _exit(ok ? 0 : 12);
  }

  RingSlotReservation slot0{};
  RingSlotReservation slot1{};
  RingSlotReservation slot2{};
  std::uint32_t si0 = 0;
  std::uint32_t si1 = 0;
  std::uint32_t si2 = 0;
  if (!Check(
          ring->ReservePublishSlotRingOrdered(8, &slot0, &si0) && si0 == 0 && ring->CommitSlot(si0),
          "commit slot0 should succeed")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (!Check(
          ring->ReservePublishSlotRingOrdered(8, &slot1, &si1) && si1 == 1 && ring->CommitSlot(si1),
          "commit slot1 should succeed")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (!Check(
          ring->ReservePublishSlotRingOrdered(8, &slot2, &si2) && si2 == 2 && ring->CommitSlot(si2),
          "commit slot2 should succeed")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (!Check(ring->AdvanceConsumerCursor(0, 4), "fast consumer reaches latest sequence")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (!Check(
          WaitForCondition(
              [&]() {
                const auto cursor = ring->LoadConsumerCursor(1);
                return cursor.has_value() && *cursor == 2;
              },
              300),
          "slow consumer cursor should be visible from child process")) {
    shm_unlink(shm_name.c_str());
    return false;
  }

  const auto fast_lag = ring->ConsumerLag(0);
  const auto slow_lag = ring->ConsumerLag(1);
  int status = 0;
  const bool child_ok = waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  const bool ok = Check(child_ok, "child process should complete lag setup") &&
      Check(fast_lag.has_value() && *fast_lag == 0, "fast consumer lag should be zero at head") &&
      Check(slow_lag.has_value() && *slow_lag == 2, "slow consumer lag should expose backlog size") &&
      Check(
          ring->MinActiveReadSequence().has_value() && *ring->MinActiveReadSequence() == 2,
          "slow consumer cursor should be isolated as min active sequence");
  shm_unlink(shm_name.c_str());
  return ok;
}

bool TestCompleteConsumerSlotReclaimsSafely() {
  const std::size_t layout_bytes = ComputeRingLayoutSizeBytes(2, 2);
  std::vector<std::uint8_t> backing(layout_bytes + 128, 0);
  auto ring = RingLayoutView::Initialize(backing.data(), backing.size(), 2, 2, 128);
  if (!Check(ring.has_value(), "ring initialize should succeed for complete-slot reclaim")) {
    return false;
  }
  if (!Check(ring->RegisterConsumer(0, 1), "consumer-0 register should succeed")) {
    return false;
  }
  if (!Check(ring->RegisterConsumer(1, 1), "consumer-1 register should succeed")) {
    return false;
  }

  RingSlotReservation reservation{};
  std::uint32_t slot_index = 0;
  if (!Check(ring->ReservePublishSlotRingOrdered(8, &reservation, &slot_index) && slot_index == 0, "reserve should succeed")) {
    return false;
  }
  if (!Check(ring->CommitSlot(slot_index), "commit should succeed")) {
    return false;
  }
  if (!Check(ring->AdvanceConsumerCursor(1, 2), "peer consumer should advance first")) {
    return false;
  }

  if (!Check(
          ring->CompleteConsumerSlot(0, 0, 2),
          "consumer completion should advance cursor and reclaim slot when safe")) {
    return false;
  }
  return Check(!ring->TryReadCommitted(0, &reservation), "reclaimed slot should no longer be committed");
}

bool TestContinuousProduceConsumeWithIntermediateChecks() {
  constexpr std::uint32_t slot_count = 16;
  constexpr std::uint32_t consumer_capacity = 1;
  constexpr std::uint64_t payload_bytes = 1024;
  constexpr std::uint32_t message_bytes = 32;

  const std::size_t layout_bytes = ComputeRingLayoutSizeBytes(slot_count, consumer_capacity);
  std::vector<std::uint8_t> backing(layout_bytes + payload_bytes, 0);
  auto ring = RingLayoutView::Initialize(backing.data(), backing.size(), slot_count, consumer_capacity, payload_bytes);
  if (!Check(ring.has_value(), "ring initialize should succeed for continuous produce/consume")) {
    return false;
  }
  if (!Check(ring->RegisterConsumer(0, 1), "consumer register should succeed")) {
    return false;
  }

  std::atomic<bool> stop{false};
  std::atomic<bool> producer_done{false};
  std::atomic<int> failures{0};
  std::atomic<int> payload_mismatch_failures{0};
  std::atomic<int> duplicate_sequence_failures{0};
  std::atomic<int> complete_slot_failures{0};
  std::atomic<int> accounting_failures{0};
  std::atomic<std::uint64_t> produced{0};
  std::atomic<std::uint64_t> consumed{0};

  std::thread producer([&]() {
    while (!stop.load(std::memory_order_acquire)) {
      RingSlotReservation reservation{};
      std::uint32_t slot_index = 0;
      if (!ring->ReservePublishSlotRingOrdered(message_bytes, &reservation, &slot_index)) {
        std::this_thread::yield();
        continue;
      }
      const std::uint64_t payload_offset = reservation.payload_offset;
      const std::uint8_t marker = static_cast<std::uint8_t>((reservation.sequence % 251U) + 1U);
      std::uint8_t* payload = ring->PayloadRegion();
      for (std::uint32_t i = 0; i < message_bytes; ++i) {
        payload[payload_offset + i] = marker;
      }
      if (!ring->CommitSlot(slot_index)) {
        failures.fetch_add(1, std::memory_order_relaxed);
        break;
      }
      produced.fetch_add(1, std::memory_order_release);
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::thread consumer([&]() {
    while (true) {
      const auto expected_read = ring->LoadConsumerCursor(0);
      if (!expected_read.has_value()) {
        complete_slot_failures.fetch_add(1, std::memory_order_relaxed);
        failures.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      const std::uint64_t expected_sequence = *expected_read;
      const std::uint32_t slot_index = static_cast<std::uint32_t>((expected_sequence - 1U) % slot_count);

      RingSlotReservation committed{};
      if (ring->TryReadCommitted(slot_index, &committed)) {
        if (committed.sequence != expected_sequence) {
          duplicate_sequence_failures.fetch_add(1, std::memory_order_relaxed);
          failures.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        const std::uint8_t expected_marker = static_cast<std::uint8_t>((committed.sequence % 251U) + 1U);
        const std::uint8_t* payload = ring->PayloadRegion();
        for (std::uint32_t i = 0; i < committed.payload_size; ++i) {
          if (payload[committed.payload_offset + i] != expected_marker) {
            payload_mismatch_failures.fetch_add(1, std::memory_order_relaxed);
            failures.fetch_add(1, std::memory_order_relaxed);
            return;
          }
        }
        if (!ring->CompleteConsumerSlot(0, slot_index, expected_sequence + 1U)) {
          complete_slot_failures.fetch_add(1, std::memory_order_relaxed);
          failures.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        consumed.fetch_add(1, std::memory_order_release);
      }

      const std::uint64_t produced_now = produced.load(std::memory_order_acquire);
      const std::uint64_t consumed_now = consumed.load(std::memory_order_acquire);
      if (producer_done.load(std::memory_order_acquire) && consumed_now == produced_now) {
        return;
      }
      std::this_thread::yield();
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  stop.store(true, std::memory_order_release);
  producer.join();
  consumer.join();

  const std::uint64_t produced_total = produced.load(std::memory_order_acquire);
  const std::uint64_t consumed_total = consumed.load(std::memory_order_acquire);
  LOG(INFO) << "[continuous-stats] produced=" << produced_total
            << " consumed=" << consumed_total
            << " failures=" << failures.load(std::memory_order_relaxed)
            << " payload_mismatch=" << payload_mismatch_failures.load(std::memory_order_relaxed)
            << " duplicate_sequence=" << duplicate_sequence_failures.load(std::memory_order_relaxed)
            << " complete_slot=" << complete_slot_failures.load(std::memory_order_relaxed)
            << " accounting=" << accounting_failures.load(std::memory_order_relaxed);
  if (!Check(failures.load(std::memory_order_relaxed) == 0, "continuous run should not hit invariant failures")) {
    return false;
  }
  if (!Check(produced_total > 0, "continuous run should produce at least one message")) {
    return false;
  }
  if (!Check(consumed_total == produced_total, "all produced messages should be consumed")) {
    return false;
  }
  if (!Check(
          ring->LoadConsumerCursor(0).has_value() && *ring->LoadConsumerCursor(0) == produced_total + 1U,
          "consumer cursor should advance to produced+1")) {
    return false;
  }
  return Check(ring->ReclaimCommittedSlots() == 0, "no residual committed slots should remain after full consumption");
}

bool TestContinuousProduceConsumeCrossProcess() {
  constexpr std::uint32_t slot_count = 32;
  constexpr std::uint32_t consumer_capacity = 1;
  constexpr std::uint64_t payload_bytes = 4096;
  constexpr std::uint32_t message_bytes = 64;
  constexpr std::uint64_t total_messages = 3000;

  const std::string channel = BuildUniqueChannelName("continuous_cross_process");
  const std::string shm_name = BuildDeterministicShmName(channel);
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
  if (!Check(ring.has_value(), "parent should initialize ring for cross-process continuous test")) {
    shm_unlink(shm_name.c_str());
    return false;
  }

  pid_t pid = fork();
  if (!Check(pid >= 0, "fork should succeed")) {
    shm_unlink(shm_name.c_str());
    return false;
  }

  if (pid == 0) {
    auto child_segment = ShmSegment::CreateOrAttach(channel, layout);
    auto child_ring = child_segment ? AttachRing(&(*child_segment)) : std::nullopt;
    if (!child_ring.has_value() || !child_ring->RegisterConsumer(0, 1)) {
      _exit(20);
    }

    for (std::uint64_t expected_sequence = 1; expected_sequence <= total_messages; ++expected_sequence) {
      const std::uint32_t slot_index = static_cast<std::uint32_t>((expected_sequence - 1U) % slot_count);
      RingSlotReservation committed{};
      while (!child_ring->TryReadCommitted(slot_index, &committed)) {
        std::this_thread::yield();
      }
      if (committed.sequence != expected_sequence) {
        _exit(21);
      }
      const std::uint8_t expected_marker = static_cast<std::uint8_t>((committed.sequence % 251U) + 1U);
      const std::uint8_t* payload = child_ring->PayloadRegion();
      for (std::uint32_t i = 0; i < committed.payload_size; ++i) {
        if (payload[committed.payload_offset + i] != expected_marker) {
          _exit(22);
        }
      }
      if (!child_ring->CompleteConsumerSlot(0, slot_index, expected_sequence + 1U)) {
        _exit(23);
      }
      if (expected_sequence % 256U == 0U) {
        const auto cursor = child_ring->LoadConsumerCursor(0);
        if (!cursor.has_value() || *cursor != expected_sequence + 1U) {
          _exit(24);
        }
      }
    }
    _exit(0);
  }

  for (std::uint64_t produced = 0; produced < total_messages; ++produced) {
    RingSlotReservation reservation{};
    std::uint32_t slot_index = 0;
    while (!ring->ReservePublishSlotRingOrdered(message_bytes, &reservation, &slot_index)) {
      std::this_thread::yield();
    }
    const std::uint64_t payload_offset = reservation.payload_offset;
    const std::uint8_t marker = static_cast<std::uint8_t>((reservation.sequence % 251U) + 1U);
    std::uint8_t* payload = ring->PayloadRegion();
    for (std::uint32_t i = 0; i < message_bytes; ++i) {
      payload[payload_offset + i] = marker;
    }
    if (!Check(ring->CommitSlot(slot_index), "producer commit should succeed in cross-process run")) {
      shm_unlink(shm_name.c_str());
      return false;
    }
    if ((produced + 1U) % 512U == 0U) {
      const std::uint64_t next_sequence = ring->Header()->next_sequence.load(std::memory_order_acquire);
      if (!Check(next_sequence > produced, "next_sequence should progress during cross-process run")) {
        shm_unlink(shm_name.c_str());
        return false;
      }
    }
  }

  int status = 0;
  const bool wait_ok = waitpid(pid, &status, 0) == pid;
  const bool child_ok = wait_ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (!Check(child_ok, "child continuous consumer should exit successfully")) {
    shm_unlink(shm_name.c_str());
    return false;
  }

  const auto cursor = ring->LoadConsumerCursor(0);
  const bool ok = Check(
                      cursor.has_value() && *cursor == total_messages + 1U,
                      "consumer cursor should reach total_messages+1 after run") &&
      Check(
          ring->Header()->next_sequence.load(std::memory_order_acquire) == total_messages + 1U,
          "producer next_sequence should match produced total") &&
      Check(ring->ReclaimCommittedSlots() == 0, "cross-process run should not leave reclaimable committed slots");
  shm_unlink(shm_name.c_str());
  return ok;
}

bool TestContinuousProduceConsumeMultiConsumerCrossProcess() {
  constexpr std::uint32_t slot_count = 32;
  constexpr std::uint32_t consumer_capacity = 4;
  constexpr std::uint32_t active_consumers = 3;
  constexpr std::uint64_t payload_bytes = 4096;
  constexpr std::uint32_t message_bytes = 64;
  constexpr std::uint64_t total_messages = 2000;
  constexpr auto per_message_throttle = std::chrono::milliseconds(5);

  const std::string channel = BuildUniqueChannelName("continuous_multi_consumer");
  const std::string shm_name = BuildDeterministicShmName(channel);
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
  if (!Check(ring.has_value(), "parent should initialize ring for multi-consumer cross-process test")) {
    shm_unlink(shm_name.c_str());
    return false;
  }

  std::vector<pid_t> child_pids;
  child_pids.reserve(active_consumers);
  std::vector<bool> child_reaped(active_consumers, false);
  std::vector<bool> child_success(active_consumers, false);
  std::vector<int> child_statuses(active_consumers, 0);
  for (std::uint32_t consumer_index = 0; consumer_index < active_consumers; ++consumer_index) {
    pid_t pid = fork();
    if (!Check(pid >= 0, "fork should succeed for each consumer")) {
      shm_unlink(shm_name.c_str());
      return false;
    }
    if (pid == 0) {
      auto child_segment = ShmSegment::CreateOrAttach(channel, layout);
      auto child_ring = child_segment ? AttachRing(&(*child_segment)) : std::nullopt;
      const auto exit_with_cleanup = [&](int code) {
        if (code != 0 && child_ring.has_value()) {
          (void)child_ring->UnregisterConsumer(consumer_index);
        }
        _exit(code);
      };
      if (!child_ring.has_value() || !child_ring->RegisterConsumer(consumer_index, 1)) {
        exit_with_cleanup(30);
      }

      for (std::uint64_t expected_sequence = 1; expected_sequence <= total_messages; ++expected_sequence) {
        const std::uint32_t slot_index = static_cast<std::uint32_t>((expected_sequence - 1U) % slot_count);
        RingSlotReservation committed{};
        while (!child_ring->TryReadCommitted(slot_index, &committed)) {
          std::this_thread::yield();
        }
        if (committed.sequence != expected_sequence) {
          exit_with_cleanup(31);
        }
        const std::uint8_t expected_marker = static_cast<std::uint8_t>((committed.sequence % 251U) + 1U);
        const std::uint8_t* payload = child_ring->PayloadRegion();
        for (std::uint32_t i = 0; i < committed.payload_size; ++i) {
          if (payload[committed.payload_offset + i] != expected_marker) {
            exit_with_cleanup(32);
          }
        }
        if (!child_ring->CompleteConsumerSlot(consumer_index, slot_index, expected_sequence + 1U)) {
          exit_with_cleanup(33);
        }
        if (expected_sequence % 400U == 0U) {
          const auto cursor = child_ring->LoadConsumerCursor(consumer_index);
          if (!cursor.has_value() || *cursor != expected_sequence + 1U) {
            exit_with_cleanup(34);
          }
        }
      }
      exit_with_cleanup(0);
    }
    child_pids.push_back(pid);
  }

  const auto cleanup_children = [&]() {
    for (std::size_t i = 0; i < child_pids.size(); ++i) {
      if (child_reaped[i]) {
        continue;
      }
      (void)kill(child_pids[i], SIGKILL);
      int status = 0;
      if (waitpid(child_pids[i], &status, 0) == child_pids[i]) {
        child_reaped[i] = true;
        child_statuses[i] = status;
        child_success[i] = WIFEXITED(status) && WEXITSTATUS(status) == 0;
      }
    }
  };

  if (!Check(
          WaitForCondition(
              [&]() {
                for (std::uint32_t i = 0; i < active_consumers; ++i) {
                  if (!ring->LoadConsumerCursor(i).has_value()) {
                    return false;
                  }
                }
                return true;
              },
              1000),
          "all consumer child processes should register before producer starts")) {
    cleanup_children();
    shm_unlink(shm_name.c_str());
    return false;
  }

  for (std::uint64_t produced = 0; produced < total_messages; ++produced) {
    RingSlotReservation reservation{};
    std::uint32_t slot_index = 0;
    const auto reserve_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (!ring->ReservePublishSlotRingOrdered(message_bytes, &reservation, &slot_index)) {
      bool child_failed = false;
      for (std::size_t i = 0; i < child_pids.size(); ++i) {
        if (child_reaped[i]) {
          if (!child_success[i]) {
            child_failed = true;
          }
          continue;
        }
        int status = 0;
        const pid_t wait_result = waitpid(child_pids[i], &status, WNOHANG);
        if (wait_result == child_pids[i]) {
          child_reaped[i] = true;
          child_statuses[i] = status;
          child_success[i] = WIFEXITED(status) && WEXITSTATUS(status) == 0;
          if (!child_success[i]) {
            child_failed = true;
          }
        }
      }
      if (child_failed || std::chrono::steady_clock::now() >= reserve_deadline) {
        LOG(ERROR) << "[multi-consumer-timeout] produced=" << produced
                   << " next_sequence=" << ring->Header()->next_sequence.load(std::memory_order_acquire)
                   << " reclaim_sequence=" << ring->Header()->reclaim_sequence.load(std::memory_order_acquire);
        for (std::uint32_t i = 0; i < active_consumers; ++i) {
          const auto cursor = ring->LoadConsumerCursor(i);
          const bool active = ring->Consumers()[i].state.load(std::memory_order_acquire) ==
              static_cast<std::uint32_t>(mould::comm::ConsumerState::kOnline);
          LOG(ERROR) << "  consumer[" << i << "] active=" << (active ? 1 : 0)
                     << " cursor=" << (cursor.has_value() ? std::to_string(*cursor) : "null");
        }
        cleanup_children();
        shm_unlink(shm_name.c_str());
        return false;
      }
      std::this_thread::yield();
    }
    const std::uint64_t payload_offset = reservation.payload_offset;
    const std::uint8_t marker = static_cast<std::uint8_t>((reservation.sequence % 251U) + 1U);
    std::uint8_t* payload = ring->PayloadRegion();
    for (std::uint32_t i = 0; i < message_bytes; ++i) {
      payload[payload_offset + i] = marker;
    }
    if (!Check(ring->CommitSlot(slot_index), "producer commit should succeed in multi-consumer run")) {
      shm_unlink(shm_name.c_str());
      return false;
    }
    std::this_thread::sleep_for(per_message_throttle);
  }

  bool all_children_ok = true;
  for (std::size_t i = 0; i < child_pids.size(); ++i) {
    if (!child_reaped[i]) {
      int status = 0;
      const bool waited = waitpid(child_pids[i], &status, 0) == child_pids[i];
      child_reaped[i] = waited;
      child_statuses[i] = status;
      child_success[i] = waited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    if (!child_success[i]) {
      all_children_ok = false;
    }
  }
  if (!Check(all_children_ok, "all consumer child processes should exit successfully")) {
    shm_unlink(shm_name.c_str());
    return false;
  }

  bool cursors_ok = true;
  for (std::uint32_t consumer_index = 0; consumer_index < active_consumers; ++consumer_index) {
    const auto cursor = ring->LoadConsumerCursor(consumer_index);
    cursors_ok = cursors_ok && cursor.has_value() && *cursor == total_messages + 1U;
  }

  const bool ok = Check(cursors_ok, "all active consumers should advance cursor to total_messages+1") &&
      Check(
          ring->Header()->next_sequence.load(std::memory_order_acquire) == total_messages + 1U,
          "producer next_sequence should match produced total in multi-consumer run") &&
      Check(ring->ReclaimCommittedSlots() == 0, "multi-consumer run should not leave reclaimable committed slots");
  shm_unlink(shm_name.c_str());
  return ok;
}

bool TestBackpressureResumeWithSlowConsumerCrossProcess() {
  constexpr std::uint32_t slot_count = 8;
  constexpr std::uint32_t consumer_capacity = 2;
  constexpr std::uint64_t payload_bytes = 2048;
  constexpr std::uint32_t message_bytes = 64;
  constexpr std::uint64_t warmup_messages = slot_count;
  constexpr std::uint64_t tail_messages = 80;
  constexpr auto slow_start_delay = std::chrono::milliseconds(200);
  constexpr auto reserve_timeout = std::chrono::milliseconds(2000);
  constexpr auto min_observed_backpressure_wait = std::chrono::milliseconds(80);
  constexpr auto tail_produce_throttle = std::chrono::milliseconds(1);

  const std::string channel = BuildUniqueChannelName("backpressure_resume");
  const std::string shm_name = BuildDeterministicShmName(channel);
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
  if (!Check(ring.has_value(), "parent should initialize ring for backpressure resume test")) {
    shm_unlink(shm_name.c_str());
    return false;
  }

  auto produce_one = [&](std::uint64_t /*produced_index*/) -> bool {
    RingSlotReservation reservation{};
    std::uint32_t slot_index = 0;
    const auto deadline = std::chrono::steady_clock::now() + reserve_timeout;
    while (!ring->ReservePublishSlotRingOrdered(message_bytes, &reservation, &slot_index)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::yield();
    }
    const std::uint64_t payload_offset = reservation.payload_offset;
    const std::uint8_t marker = static_cast<std::uint8_t>((reservation.sequence % 251U) + 1U);
    std::uint8_t* payload = ring->PayloadRegion();
    for (std::uint32_t i = 0; i < message_bytes; ++i) {
      payload[payload_offset + i] = marker;
    }
    return ring->CommitSlot(slot_index);
  };

  pid_t slow_pid = fork();
  if (!Check(slow_pid >= 0, "fork should succeed for slow consumer")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (slow_pid == 0) {
    auto child_segment = ShmSegment::CreateOrAttach(channel, layout);
    auto child_ring = child_segment ? AttachRing(&(*child_segment)) : std::nullopt;
    const auto exit_with_cleanup = [&](int code) {
      if (code != 0 && child_ring.has_value()) {
        (void)child_ring->UnregisterConsumer(0);
      }
      _exit(code);
    };
    if (!child_ring.has_value() || !child_ring->RegisterConsumer(0, 1)) {
      exit_with_cleanup(40);
    }
    std::this_thread::sleep_for(slow_start_delay);
    for (std::uint64_t expected_sequence = 1; expected_sequence <= warmup_messages + tail_messages; ++expected_sequence) {
      const std::uint32_t slot_index = static_cast<std::uint32_t>((expected_sequence - 1U) % slot_count);
      RingSlotReservation committed{};
      while (!child_ring->TryReadCommitted(slot_index, &committed)) {
        std::this_thread::yield();
      }
      if (committed.sequence < expected_sequence) {
        --expected_sequence;
        std::this_thread::yield();
        continue;
      }
      if (committed.sequence > expected_sequence) {
        exit_with_cleanup(41);
      }
      if (!child_ring->CompleteConsumerSlot(0, slot_index, expected_sequence + 1U)) {
        exit_with_cleanup(42);
      }
    }
    exit_with_cleanup(0);
  }

  pid_t fast_pid = fork();
  if (!Check(fast_pid >= 0, "fork should succeed for fast consumer")) {
    (void)kill(slow_pid, SIGKILL);
    int status = 0;
    (void)waitpid(slow_pid, &status, 0);
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (fast_pid == 0) {
    auto child_segment = ShmSegment::CreateOrAttach(channel, layout);
    auto child_ring = child_segment ? AttachRing(&(*child_segment)) : std::nullopt;
    const auto exit_with_cleanup = [&](int code) {
      if (code != 0 && child_ring.has_value()) {
        (void)child_ring->UnregisterConsumer(1);
      }
      _exit(code);
    };
    if (!child_ring.has_value() || !child_ring->RegisterConsumer(1, 1)) {
      exit_with_cleanup(50);
    }
    for (std::uint64_t expected_sequence = 1; expected_sequence <= warmup_messages + tail_messages; ++expected_sequence) {
      const std::uint32_t slot_index = static_cast<std::uint32_t>((expected_sequence - 1U) % slot_count);
      RingSlotReservation committed{};
      while (!child_ring->TryReadCommitted(slot_index, &committed)) {
        std::this_thread::yield();
      }
      if (committed.sequence < expected_sequence) {
        --expected_sequence;
        std::this_thread::yield();
        continue;
      }
      if (committed.sequence > expected_sequence) {
        exit_with_cleanup(51);
      }
      if (!child_ring->CompleteConsumerSlot(1, slot_index, expected_sequence + 1U)) {
        exit_with_cleanup(52);
      }
    }
    exit_with_cleanup(0);
  }

  if (!Check(
          WaitForCondition(
              [&]() {
                return ring->LoadConsumerCursor(0).has_value() && ring->LoadConsumerCursor(1).has_value();
              },
              1000),
          "both consumers should register before producer starts")) {
    (void)kill(slow_pid, SIGKILL);
    (void)kill(fast_pid, SIGKILL);
    int status = 0;
    (void)waitpid(slow_pid, &status, 0);
    (void)waitpid(fast_pid, &status, 0);
    shm_unlink(shm_name.c_str());
    return false;
  }

  bool warmup_ok = true;
  for (std::uint64_t produced = 0; produced < warmup_messages; ++produced) {
    warmup_ok = warmup_ok && produce_one(produced);
  }
  if (!Check(warmup_ok, "producer should fill initial window before backpressure")) {
    (void)kill(slow_pid, SIGKILL);
    (void)kill(fast_pid, SIGKILL);
    int status = 0;
    (void)waitpid(slow_pid, &status, 0);
    (void)waitpid(fast_pid, &status, 0);
    shm_unlink(shm_name.c_str());
    return false;
  }

  const auto first_tail_start = std::chrono::steady_clock::now();
  const bool first_tail_ok = produce_one(warmup_messages);
  const auto first_tail_elapsed = std::chrono::steady_clock::now() - first_tail_start;
  if (!Check(first_tail_ok, "first tail publish should eventually succeed after backpressure")) {
    (void)kill(slow_pid, SIGKILL);
    (void)kill(fast_pid, SIGKILL);
    int status = 0;
    (void)waitpid(slow_pid, &status, 0);
    (void)waitpid(fast_pid, &status, 0);
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (!Check(
          first_tail_elapsed >= min_observed_backpressure_wait,
          "producer should observe measurable wait under full-buffer backpressure")) {
    (void)kill(slow_pid, SIGKILL);
    (void)kill(fast_pid, SIGKILL);
    int status = 0;
    (void)waitpid(slow_pid, &status, 0);
    (void)waitpid(fast_pid, &status, 0);
    shm_unlink(shm_name.c_str());
    return false;
  }
  std::this_thread::sleep_for(tail_produce_throttle);

  bool tail_ok = true;
  for (std::uint64_t produced = warmup_messages + 1U; produced < warmup_messages + tail_messages; ++produced) {
    tail_ok = tail_ok && produce_one(produced);
    std::this_thread::sleep_for(tail_produce_throttle);
  }
  if (!Check(tail_ok, "producer should resume after slow consumer catches up")) {
    (void)kill(slow_pid, SIGKILL);
    (void)kill(fast_pid, SIGKILL);
    int status = 0;
    (void)waitpid(slow_pid, &status, 0);
    (void)waitpid(fast_pid, &status, 0);
    shm_unlink(shm_name.c_str());
    return false;
  }

  int slow_status = 0;
  int fast_status = 0;
  const bool slow_ok = waitpid(slow_pid, &slow_status, 0) == slow_pid && WIFEXITED(slow_status) &&
      WEXITSTATUS(slow_status) == 0;
  const bool fast_ok = waitpid(fast_pid, &fast_status, 0) == fast_pid && WIFEXITED(fast_status) &&
      WEXITSTATUS(fast_status) == 0;
  if (!(slow_ok && fast_ok)) {
    LOG(ERROR) << "[backpressure-child-status] slow_status=" << slow_status
               << " fast_status=" << fast_status;
  }
  if (!Check(slow_ok && fast_ok, "both consumer processes should complete after backpressure resumes")) {
    shm_unlink(shm_name.c_str());
    return false;
  }

  const std::uint64_t expected_next = warmup_messages + tail_messages + 1U;
  const auto slow_cursor = ring->LoadConsumerCursor(0);
  const auto fast_cursor = ring->LoadConsumerCursor(1);
  const bool ok = Check(
                      slow_cursor.has_value() && fast_cursor.has_value() &&
                          *slow_cursor == expected_next && *fast_cursor == expected_next,
                      "both cursors should reach the same terminal sequence after resume") &&
      Check(
          ring->Header()->next_sequence.load(std::memory_order_acquire) == expected_next,
          "producer sequence should remain lossless under backpressure and resume");
  shm_unlink(shm_name.c_str());
  return ok;
}

bool TestDedupKeyUsesChannelAndSequence() {
  MessageEnvelope first;
  first.channel = "infer.input";
  first.sequence = 42;

  MessageEnvelope same = first;
  MessageEnvelope diff_channel = first;
  diff_channel.channel = "infer.output";
  MessageEnvelope diff_sequence = first;
  diff_sequence.sequence = 43;

  const std::string first_key = ShmBusRuntime::BuildDedupKey(first);
  const std::string same_key = ShmBusRuntime::BuildDedupKey(same);
  const std::string diff_channel_key = ShmBusRuntime::BuildDedupKey(diff_channel);
  const std::string diff_sequence_key = ShmBusRuntime::BuildDedupKey(diff_sequence);

  if (!Check(!first_key.empty(), "dedup key should be generated for valid channel+sequence")) {
    return false;
  }
  if (!Check(first_key == same_key, "same channel+sequence should produce stable dedup key")) {
    return false;
  }
  return Check(
      first_key != diff_channel_key && first_key != diff_sequence_key,
      "different channel or sequence should produce different dedup key");
}

bool TestDedupKeyRejectsInvalidInputs() {
  MessageEnvelope missing_channel;
  missing_channel.sequence = 7;
  if (!Check(
          ShmBusRuntime::BuildDedupKey(missing_channel).empty(),
          "dedup key should be empty when channel is missing")) {
    return false;
  }

  MessageEnvelope zero_sequence;
  zero_sequence.channel = "infer.input";
  zero_sequence.sequence = 0;
  return Check(
      ShmBusRuntime::BuildDedupKey(zero_sequence).empty(),
      "dedup key should be empty when sequence is zero");
}

}  // namespace

static int RunLegacyMultiConsumerCursorReclaimSuite() {
  bool ok = true;
  ok = TestConsumerCursorRegisterAdvanceAndPersistCrossProcess() && ok;
  ok = TestReclaimUsesMinActiveCursorCrossProcess() && ok;
  ok = TestSlowConsumerLagAndIsolationCrossProcess() && ok;
  ok = TestCompleteConsumerSlotReclaimsSafely() && ok;
  ok = TestContinuousProduceConsumeWithIntermediateChecks() && ok;
  ok = TestContinuousProduceConsumeCrossProcess() && ok;
  ok = TestContinuousProduceConsumeMultiConsumerCrossProcess() && ok;
  ok = TestBackpressureResumeWithSlowConsumerCrossProcess() && ok;
  ok = TestDedupKeyUsesChannelAndSequence() && ok;
  ok = TestDedupKeyRejectsInvalidInputs() && ok;
  return ok ? 0 : 1;
}

TEST(MultiConsumerCursorReclaim, LegacyHistoricalCrossProcessSuite) {
  EXPECT_EQ(RunLegacyMultiConsumerCursorReclaimSuite(), 0);
}

TEST(CompeteDeliveryModeConfig, ResolveDeliveryModeAndValidation) {
  using mould::config::ChannelTopologyEntry;
  using mould::config::IsValidShmBusDeliveryModeParamValue;
  using mould::config::ResolveShmBusDeliveryModeForChannel;
  using mould::config::ShmBusDeliveryMode;

  ChannelTopologyEntry e;
  EXPECT_EQ(ResolveShmBusDeliveryModeForChannel(&e), ShmBusDeliveryMode::kBroadcast);
  e.params["delivery_mode"] = "CoMpEtE";
  EXPECT_EQ(ResolveShmBusDeliveryModeForChannel(&e), ShmBusDeliveryMode::kCompete);
  EXPECT_TRUE(IsValidShmBusDeliveryModeParamValue(""));
  EXPECT_TRUE(IsValidShmBusDeliveryModeParamValue("broadcast"));
  EXPECT_FALSE(IsValidShmBusDeliveryModeParamValue("invalid_mode"));
}

TEST(CompeteDeliveryMode, ErrorHandlingAndFallback) {
  constexpr std::uint32_t slot_count = 4;
  constexpr std::uint32_t consumer_capacity = 2;
  const std::size_t layout_bytes = mould::comm::ComputeRingLayoutSizeBytes(slot_count, consumer_capacity);
  std::vector<std::uint8_t> backing(layout_bytes + 256, 0);
  auto ring = mould::comm::RingLayoutView::Initialize(
      backing.data(), backing.size(), slot_count, consumer_capacity, 256);
  ASSERT_TRUE(ring.has_value());
  ASSERT_TRUE(ring->RegisterConsumer(0, 1));

  // TryClaimSlot with invalid slot_index returns false.
  EXPECT_FALSE(ring->TryClaimSlot(slot_count));
  EXPECT_FALSE(ring->TryClaimSlot(slot_count + 100));

  // Publish one message.
  RingSlotReservation res{};
  std::uint32_t si = 0;
  ASSERT_TRUE(ring->ReservePublishSlotRingOrdered(8, &res, &si));
  ASSERT_TRUE(ring->CommitSlot(si));

  // First claim succeeds.
  EXPECT_TRUE(ring->TryClaimSlot(si));
  // Second claim on same slot fails (already claimed).
  EXPECT_FALSE(ring->TryClaimSlot(si));

  // After claim (regardless of handler outcome), CompleteConsumerSlot advances cursor.
  ASSERT_TRUE(ring->CompleteConsumerSlot(0, si, 2));
  auto cursor = ring->LoadConsumerCursor(0);
  ASSERT_TRUE(cursor.has_value());
  EXPECT_EQ(*cursor, 2U);
}

TEST(CompeteDeliveryMode, CursorSynchronizationAfterConsume) {
  constexpr std::uint32_t slot_count = 4;
  constexpr std::uint32_t consumer_capacity = 2;
  const std::size_t layout_bytes = mould::comm::ComputeRingLayoutSizeBytes(slot_count, consumer_capacity);
  std::vector<std::uint8_t> backing(layout_bytes + 256, 0);
  auto ring = mould::comm::RingLayoutView::Initialize(backing.data(), backing.size(), slot_count, consumer_capacity, 256);
  ASSERT_TRUE(ring.has_value());
  ASSERT_TRUE(ring->RegisterConsumer(0, 1));
  ASSERT_TRUE(ring->RegisterConsumer(1, 1));

  RingSlotReservation res{};
  std::uint32_t si = 0;
  ASSERT_TRUE(ring->ReservePublishSlotRingOrdered(8, &res, &si));
  ASSERT_TRUE(ring->CommitSlot(si));

  ASSERT_TRUE(ring->CompleteConsumerSlot(0, si, 2));
  ASSERT_TRUE(ring->AdvanceConsumerCursor(1, 2));

  EXPECT_EQ(ring->ReclaimCommittedSlots(), 1U);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
