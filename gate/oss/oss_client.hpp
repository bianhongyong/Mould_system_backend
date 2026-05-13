#pragma once

#include <chrono>
#include <cstdlib>
#include <string>
#include <unordered_map>

#include <memory>

#include "iblob_storage_uploader.hpp"
#include "thread_pool.hpp"

namespace mould {
namespace gate {
namespace oss {

/// 基于阿里云 OSS SDK 的 IBlobStorageUploader 实现。
///
/// 凭证通过 Options::FromEnv() 从环境变量读取。
///
/// 环境变量：
///   MOULD_OSS_ACCESS_KEY_ID      — AccessKey ID
///   MOULD_OSS_ACCESS_KEY_SECRET  — AccessKey Secret
///   MOULD_OSS_ENDPOINT           — OSS Endpoint
///   MOULD_OSS_BUCKET             — Bucket 名称
///   MOULD_OSS_MAX_RETRIES        — 最大重试次数（默认 3）
///   MOULD_OSS_BASE_BACKOFF_MS    — 指数退避基数毫秒（默认 1000）
///
/// 重试策略：
///   - 默认最多重试 3 次
///   - 指数退避：1s、2s、4s
///   - 仅对可重试错误（网络超时、服务端 5xx）重试
class OssClient : public IBlobStorageUploader {
 public:
  struct Options {
    /// 访问凭据（从环境变量读取）
    std::string access_key_id;
    std::string access_key_secret;
    std::string endpoint;
    std::string bucket;

    /// 重试配置
    int max_retries = 3;
    std::chrono::milliseconds base_backoff{1000};

    /// 线程池大小（上传并发数），默认 8
    int upload_threads = 8;

    /// 从环境变量读取配置并返回 Options。
    static Options FromEnv();
  };

  explicit OssClient(Options opts);
  ~OssClient() override;

  OssClient(const OssClient&) = delete;
  OssClient& operator=(const OssClient&) = delete;

  void UploadAsync(
      const std::string& image_data,
      const std::unordered_map<std::string, std::string>& metadata,
      UploadCallback callback) override;

  /// 分片上传大文件。将数据按 part_size 切块，通过 InitiateMultipartUpload /
  /// UploadPart / CompleteMultipartUpload 完成上传。
  /// @param image_data  原始图片字节
  /// @param metadata    附加元信息
  /// @param callback    上传结束回调
  /// @param part_size   每个分片大小（字节），默认 1MB
  void UploadMultipartAsync(
      const std::string& image_data,
      const std::unordered_map<std::string, std::string>& metadata,
      UploadCallback callback,
      int64_t part_size = 1 * 1024 * 1024);

  /// 判断错误是否可重试。
  static bool IsRetryable(const std::string& error_code);

 private:
  Options opts_;
  void* oss_client_ = nullptr;  ///< Alibabacloud::OSS::OssClient* (opaque)
  ThreadPool upload_pool_;
};

// ============================================================
// Options::FromEnv 实现
// ============================================================

inline OssClient::Options OssClient::Options::FromEnv() {
  Options opts;

  auto get_env = [](const char* key, const std::string& default_val) {
    const char* val = std::getenv(key);
    return val ? std::string(val) : default_val;
  };

  opts.access_key_id     = get_env("MOULD_OSS_ACCESS_KEY_ID", "");
  opts.access_key_secret = get_env("MOULD_OSS_ACCESS_KEY_SECRET", "");
  opts.endpoint          = get_env("MOULD_OSS_ENDPOINT", "");
  opts.bucket            = get_env("MOULD_OSS_BUCKET", "");

  auto retries_str = get_env("MOULD_OSS_MAX_RETRIES", "3");
  opts.max_retries = std::atoi(retries_str.c_str());
  if (opts.max_retries < 0) opts.max_retries = 0;

  auto backoff_str = get_env("MOULD_OSS_BASE_BACKOFF_MS", "1000");
  int backoff_ms = std::atoi(backoff_str.c_str());
  if (backoff_ms <= 0) backoff_ms = 1000;
  opts.base_backoff = std::chrono::milliseconds(backoff_ms);

  return opts;
}

}  // namespace oss
}  // namespace gate
}  // namespace mould
