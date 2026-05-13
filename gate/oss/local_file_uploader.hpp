#pragma once

#include <chrono>
#include <fstream>
#include <string>

#include "iblob_storage_uploader.hpp"

namespace mould {
namespace gate {
namespace oss {

/// 本地文件上传器 —— 将图片数据写入本地文件系统指定目录。
/// 不依赖网络，用于开发环境和无网测试。
class LocalFileUploader : public IBlobStorageUploader {
 public:
  /// @param output_dir  输出目录路径，文件将写入 {output_dir}/{request_id}.jpg
  explicit LocalFileUploader(std::string output_dir)
      : output_dir_(std::move(output_dir)) {}

  void UploadAsync(
      const std::string& image_data,
      const std::unordered_map<std::string, std::string>& metadata,
      UploadCallback callback) override {
    auto start_ts = std::chrono::steady_clock::now();

    UploadResult result;

    auto it = metadata.find("request_id");
    std::string request_id = (it != metadata.end()) ? it->second : "unknown";
    result.object_key = request_id + ".jpg";

    std::string file_path = output_dir_ + "/" + result.object_key;
    std::ofstream ofs(file_path, std::ios::binary);
    if (!ofs.is_open()) {
      result.success = false;
      result.error_code = "IO_ERROR";
      result.error_message = "Failed to open file for writing: " + file_path;
    } else {
      ofs.write(image_data.data(), static_cast<std::streamsize>(image_data.size()));
      if (!ofs.good()) {
        result.success = false;
        result.error_code = "IO_ERROR";
        result.error_message = "Failed to write image data to: " + file_path;
      } else {
        result.success = true;
      }
      ofs.close();
    }

    auto end_ts = std::chrono::steady_clock::now();
    result.duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_ts - start_ts).count();

    if (callback) {
      callback(result);
    }
  }

 private:
  std::string output_dir_;
};

}  // namespace oss
}  // namespace gate
}  // namespace mould
