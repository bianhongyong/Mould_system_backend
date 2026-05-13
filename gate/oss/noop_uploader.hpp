#pragma once

#include "iblob_storage_uploader.hpp"

namespace mould {
namespace gate {
namespace oss {

/// 空操作上传器 —— UploadAsync 立即通过回调返回成功。
/// 用于本地开发、集成测试及无需真实上传的场景。
class NoOpUploader : public IBlobStorageUploader {
 public:
  void UploadAsync(
      const std::string& /*image_data*/,
      const std::unordered_map<std::string, std::string>& metadata,
      UploadCallback callback) override {
    UploadResult result;
    result.success = true;
    auto it = metadata.find("request_id");
    result.object_key = "noop/" + (it != metadata.end() ? it->second : "unknown");
    result.duration_ms = 0;
    if (callback) {
      callback(result);
    }
  }
};

}  // namespace oss
}  // namespace gate
}  // namespace mould
