#pragma once

#include "channel_topology_config.hpp"
#include "shm_bus_control_plane.hpp"

#include <memory>

namespace mould::comm {

/// Process-wide prefork hook: ensures a shared `ShmBusControlPlane` exists for the parent
/// process before `fork`. Post-fork children should construct `ShmBusRuntime`
/// with this instance so attach paths do not record owner unlink names.
void ShmBusRuntimePreforkEnsureSharedControlPlane();

std::shared_ptr<ShmBusControlPlane> ShmBusRuntimeGetSharedControlPlane();

/// Call from the supervising main process after all children have exited: unlinks every
/// segment created as owner through the shared control plane. Idempotent.
void ShmBusRuntimeFinalizeSharedControlPlaneShm();

/// Stub for loading read-only topology snapshots that remain valid across fork (COW).
/// Reserved for follow-up wiring; safe no-op.
void ShmBusRuntimePreforkWarmReadOnlyTopology(const mould::config::ChannelTopologyIndex& topology);

/// Requires `MOULD_FORK_INHERITANCE_TOKEN` unless `MOULD_RELAX_FORK_ONLY_SHM_MODEL` is set (tests/dev).
/// Call from `ModuleBase::Run` for `kSingleNodeShm` to reject accidental exec/re-exec paths.
void ShmBusRuntimeAssertForkOnlyModelOrDie();

}  // namespace mould::comm
