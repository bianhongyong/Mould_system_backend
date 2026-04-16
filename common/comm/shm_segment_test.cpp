#include "ms_logging.hpp"
#include "shm_segment.hpp"
#include "test_helpers.hpp"

#include <sys/mman.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace {

using mould::comm::BuildDeterministicShmName;
using mould::comm::ShmSegment;
using mould::comm::ShmSegmentHeaderSizeBytes;
using mould::comm::ShmSegmentLayout;

bool TestBuildDeterministicShmName() {
  const std::string name = BuildDeterministicShmName("module/alpha:channel");
  return Check(name == "/mould_comm_module_alpha_channel", "shm name should sanitize unsupported characters");
}

bool TestCreateAndAttachSegment() {
  const std::string channel = "module1_create_attach_channel";
  const std::string shm_name = BuildDeterministicShmName(channel);
  shm_unlink(shm_name.c_str());

  ShmSegmentLayout layout;
  layout.payload_capacity = 128;

  auto owner = ShmSegment::CreateOrAttach(channel, layout);
  if (!Check(owner.has_value(), "owner should create shm segment")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (!Check(owner->IsOwner(), "first opener should be owner")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (!Check(owner->SizeBytes() == ShmSegmentHeaderSizeBytes() + layout.payload_capacity, "segment size should match header + payload")) {
    shm_unlink(shm_name.c_str());
    return false;
  }

  constexpr std::uint8_t kMarker = 0x5A;
  auto* owner_base = static_cast<std::uint8_t*>(owner->BaseAddress());
  owner_base[ShmSegmentHeaderSizeBytes()] = kMarker;

  auto attached = ShmSegment::CreateOrAttach(channel, layout);
  if (!Check(attached.has_value(), "second opener should attach existing shm segment")) {
    shm_unlink(shm_name.c_str());
    return false;
  }
  if (!Check(!attached->IsOwner(), "attached segment should not be owner")) {
    shm_unlink(shm_name.c_str());
    return false;
  }

  auto* attached_base = static_cast<std::uint8_t*>(attached->BaseAddress());
  if (!Check(attached_base[ShmSegmentHeaderSizeBytes()] == kMarker, "attached mapping should observe owner writes")) {
    shm_unlink(shm_name.c_str());
    return false;
  }

  owner.reset();
  attached.reset();
  shm_unlink(shm_name.c_str());
  return true;
}

bool TestAttachRejectsIncompatibleLayout() {
  const std::string channel = "module1_layout_mismatch_channel";
  const std::string shm_name = BuildDeterministicShmName(channel);
  shm_unlink(shm_name.c_str());

  ShmSegmentLayout expected_layout;
  expected_layout.payload_capacity = 64;

  auto owner = ShmSegment::CreateOrAttach(channel, expected_layout);
  if (!Check(owner.has_value(), "owner should create segment for mismatch test")) {
    shm_unlink(shm_name.c_str());
    return false;
  }

  ShmSegmentLayout mismatch_layout = expected_layout;
  mismatch_layout.version = static_cast<std::uint16_t>(expected_layout.version + 1);
  auto mismatch_attach = ShmSegment::CreateOrAttach(channel, mismatch_layout);
  if (!Check(!mismatch_attach.has_value(), "attach should fail when version mismatches")) {
    owner.reset();
    shm_unlink(shm_name.c_str());
    return false;
  }

  owner.reset();
  shm_unlink(shm_name.c_str());
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  (void)argc;
  mould::InitApplicationLogging(argv[0]);
  bool ok = true;
  ok = TestBuildDeterministicShmName() && ok;
  ok = TestCreateAndAttachSegment() && ok;
  ok = TestAttachRejectsIncompatibleLayout() && ok;

  if (!ok) {
    LOG(ERROR) << "module1 shm segment tests failed";
    mould::ShutdownApplicationLogging();
    return 1;
  }

  LOG(INFO) << "module1 shm segment tests passed";
  mould::ShutdownApplicationLogging();
  return 0;
}
