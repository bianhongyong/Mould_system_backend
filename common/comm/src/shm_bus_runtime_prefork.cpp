#include "shm_bus_runtime_prefork.hpp"

#include <glog/logging.h>

#include <cstring>
#include <cstdlib>
#include <mutex>

namespace mould::comm {
namespace {

std::mutex g_prefork_mu;
std::shared_ptr<ShmBusControlPlane> g_shared_control_plane;

}  // namespace

void ShmBusRuntimePreforkEnsureSharedControlPlane() {
  std::lock_guard<std::mutex> lock(g_prefork_mu);
  if (!g_shared_control_plane) {
    g_shared_control_plane = std::make_shared<ShmBusControlPlane>();
  }
}

std::shared_ptr<ShmBusControlPlane> ShmBusRuntimeGetSharedControlPlane() {
  std::lock_guard<std::mutex> lock(g_prefork_mu);
  return g_shared_control_plane;
}

void ShmBusRuntimePreforkWarmReadOnlyTopology(const mould::config::ChannelTopologyIndex& /*topology*/) {
  // Intentionally empty: prefork read-only snapshot will be populated when supervisor wiring lands.
}

void ShmBusRuntimeFinalizeSharedControlPlaneShm() {
  std::shared_ptr<ShmBusControlPlane> plane;
  {
    std::lock_guard<std::mutex> lock(g_prefork_mu);
    plane = std::move(g_shared_control_plane);
  }
  if (plane) {
    plane->FinalizeUnlinkManagedSegments();
  }
}

void ShmBusRuntimeAssertForkOnlyModelOrDie() {
  const char* relax = std::getenv("MOULD_RELAX_FORK_ONLY_SHM_MODEL");
  if (relax != nullptr && relax[0] != '\0' && std::strcmp(relax, "0") != 0) {
    return;
  }
  const char* token = std::getenv("MOULD_FORK_INHERITANCE_TOKEN");
  if (token == nullptr || token[0] == '\0') {
    LOG(FATAL) << "MOULD_FORK_INHERITANCE_TOKEN is required for kSingleNodeShm modules (fork-only model). "
                  "Set MOULD_RELAX_FORK_ONLY_SHM_MODEL=1 only for unit tests or non-fork dev runs.";
  }
}

}  // namespace mould::comm
