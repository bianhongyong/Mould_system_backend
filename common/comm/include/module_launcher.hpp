#pragma once

#include <string>
#include <unordered_map>

namespace mould::config {
struct ParsedModuleLaunchEntry;
}  // namespace mould::config

namespace mould::comm {

struct ModulePipeSet;
class Supervisor;

/// Fork a child process for a business module: set CPU affinity, create the
/// ModuleBase instance, send READY over the ready pipe, then Run().
/// On success returns true (the pid -> module_name mapping is tracked inside
/// Supervisor via ForkModuleProcess). On failure returns false and sets out_error.
bool LaunchOneModule(
    const mould::config::ParsedModuleLaunchEntry& module_entry,
    Supervisor* supervisor,
    const std::unordered_map<std::string, ModulePipeSet>* all_module_pipes,
    std::string* out_error);

/// Fork the log dispatch module specially. Unlike LaunchOneModule, this:
///   - passes all other modules' pipe read ends to the child via gflags
///   - uses eventfd (not ReadyPipeProtocol) for ready signalling
/// Returns the child PID on success, 0 on failure.
pid_t ForkLogModule(
    const mould::config::ParsedModuleLaunchEntry& module_entry,
    Supervisor* supervisor,
    int ready_event_fd,
    const std::unordered_map<std::string, ModulePipeSet>& all_pipes,
    std::string* out_error);

}  // namespace mould::comm
