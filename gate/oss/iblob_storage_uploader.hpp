#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace mould {
namespace gate {
namespace oss {

/// 窄接口：blob 存储上传器。
/// 编排层和业务代码仅依赖此头文件，不直接使用 OSS SDK。
class IBlobStorageUploader {
 public:
  virtual ~IBlobStorageUploader() = default;

  /// 上传结果。
  struct UploadResult {
    bool success = false;
    std::string error_code;
    std::string error_message;
    std::string object_key;
    int64_t duration_ms = 0;
  };

  /// 上传完成回调。
  using UploadCallback = std::function<void(const UploadResult&)>;

  /// 异步上传图片数据。
  /// @param image_data  原始图片字节
  /// @param metadata    附加元信息（如 request_id、node_id、image_id 等）
  /// @param callback    上传结束回调（在内部线程或事件循环中调用）
  virtual void UploadAsync(
      const std::string& image_data,
      const std::unordered_map<std::string, std::string>& metadata,
      UploadCallback callback) = 0;
};

}  // namespace oss
}  // namespace gate
}  // namespace mould
