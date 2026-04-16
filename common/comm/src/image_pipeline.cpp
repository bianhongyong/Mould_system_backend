#include "image_pipeline.hpp"

namespace mould::comm {

ImagePreprocessor::ImagePreprocessor(PreprocessBackend backend) : backend_(backend) {}

PreprocessResult ImagePreprocessor::Prepare(const SharedImageHandle& input) const {
  PreprocessResult result;
  result.handle = input;
  result.used_gpu = (backend_ == PreprocessBackend::kGpuPreferred);
  if (!result.handle.bytes) {
    result.handle.bytes = std::make_shared<ByteBuffer>();
    result.used_gpu = false;
  }
  return result;
}

OrtBridge::OrtBridge(OrtExecutionProvider provider) : provider_(provider) {}

OrtInputBinding OrtBridge::BindInput(const SharedImageHandle& handle) const {
  OrtInputBinding binding;
  binding.provider = provider_;
  binding.handle = handle;
  if (provider_ == OrtExecutionProvider::kCpu) {
    binding.uses_external_cpu_memory = true;
    binding.transfer_strategy = "external-cpu-memory";
    return binding;
  }

  binding.uses_external_cpu_memory = false;
  binding.requires_h2d_transfer = true;
  binding.transfer_strategy = "pinned-memory-async-h2d";
  return binding;
}

}  // namespace mould::comm
