#pragma once

#include "interfaces.hpp"

#include <memory>
#include <string>
#include <vector>

namespace mould::comm {

enum class PreprocessBackend {
  kGpuPreferred,
  kCpuOnly,
};

enum class OrtExecutionProvider {
  kCpu,
  kCuda,
};

struct SharedImageHandle {
  std::shared_ptr<ByteBuffer> bytes;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t channels = 0;
};

struct PreprocessResult {
  SharedImageHandle handle;
  bool used_gpu = false;
};

struct OrtInputBinding {
  OrtExecutionProvider provider = OrtExecutionProvider::kCpu;
  bool uses_external_cpu_memory = false;
  bool requires_h2d_transfer = false;
  std::string transfer_strategy;
  SharedImageHandle handle;
};

class ImagePreprocessor {
 public:
  explicit ImagePreprocessor(PreprocessBackend backend);

  PreprocessResult Prepare(const SharedImageHandle& input) const;

 private:
  PreprocessBackend backend_ = PreprocessBackend::kGpuPreferred;
};

class OrtBridge {
 public:
  explicit OrtBridge(OrtExecutionProvider provider);

  OrtInputBinding BindInput(const SharedImageHandle& handle) const;

 private:
  OrtExecutionProvider provider_ = OrtExecutionProvider::kCpu;
};

}  // namespace mould::comm
