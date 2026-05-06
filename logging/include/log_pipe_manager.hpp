#pragma once

#include <string>
#include <unordered_map>

namespace mould::comm {

struct ModulePipeSet {
  int read_fd = -1;
  int write_fd = -1;
};

/// Creates an anonymous pipe and sets both ends CLOEXEC.
/// Returns true on success, false with out_error set on failure.
bool CreateLogPipe(ModulePipeSet* out_pipe, std::string* out_error);

/// Close all write_fds except the one for keep_module.
/// Called in a child process after fork to avoid fd leaks.
void CloseOtherWriteFds(const std::string& keep_module,
                        const std::unordered_map<std::string, ModulePipeSet>& all_pipes);

/// Close all read and write fds, then clear the map.
void CloseAllPipeFds(std::unordered_map<std::string, ModulePipeSet>* pipes);

}  // namespace mould::comm
