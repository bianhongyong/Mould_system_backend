#include "oss_client.hpp"

#include <glog/logging.h>

#include <utility>

#include <alibabacloud/oss/OssClient.h>

namespace mould {
namespace gate {
namespace oss {

OssClient::OssClient(Options opts)
    : opts_(std::move(opts)),
      upload_pool_(opts_.upload_threads > 0 ? opts_.upload_threads : 1) {
  auto* client = new AlibabaCloud::OSS::OssClient(
      opts_.endpoint,
      opts_.access_key_id,
      opts_.access_key_secret,
      AlibabaCloud::OSS::ClientConfiguration());
  oss_client_ = client;
  VLOG(1) << "[OssClient] OSS client created, bucket=" << opts_.bucket;
}

OssClient::~OssClient() {
  if (oss_client_) {
    delete static_cast<AlibabaCloud::OSS::OssClient*>(oss_client_);
    oss_client_ = nullptr;
    VLOG(1) << "[OssClient] OSS client destroyed";
  }
}

bool OssClient::IsRetryable(const std::string& error_code) {
  return error_code == "RequestTimeout" ||
         error_code == "SlowDown" ||
         error_code == "ServiceUnavailable" ||
         error_code == "InternalError" ||
         error_code == "SocketException" ||
         error_code == "ConnectionTimeout" ||
         error_code == "OperationTimeoutTime";
}

void OssClient::UploadAsync(
    const std::string& image_data,
    const std::unordered_map<std::string, std::string>& metadata,
    UploadCallback callback) {
  // 投递到线程池执行，真正异步返回
  upload_pool_.Submit([this, image_data, metadata, callback]() {
    auto start_ts = std::chrono::steady_clock::now();

    auto rid_it = metadata.find("request_id");
    std::string request_id = (rid_it != metadata.end()) ? rid_it->second : "unknown";

    auto* client = static_cast<AlibabaCloud::OSS::OssClient*>(oss_client_);
    if (!client) {
      LOG(ERROR) << "[OssClient] OSS client is null, request_id=" << request_id;
      UploadResult result;
      result.success = false;
      result.error_code = "CLIENT_ERROR";
      result.error_message = "OSS client not initialized";
      auto end_ts = std::chrono::steady_clock::now();
      result.duration_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(end_ts - start_ts).count();
      if (callback) callback(result);
      return;
    }

    std::string object_key;
    auto ok_it = metadata.find("object_key");
    if (ok_it != metadata.end()) {
      object_key = ok_it->second;
    } else {
      object_key = request_id + ".jpg";
    }

    UploadResult result;
    int attempt = 0;
    const int max_retries = opts_.max_retries;

    while (attempt <= max_retries) {
      if (attempt > 0) {
        auto backoff = opts_.base_backoff * (1 << (attempt - 1));
        LOG(WARNING) << "[OssClient] Retry attempt " << attempt
                     << "/" << max_retries << " for request_id=" << request_id
                     << ", backing off " << backoff.count() << "ms";
        std::this_thread::sleep_for(backoff);
      }

      ++attempt;

      auto content_stream =
          std::make_shared<std::stringstream>(image_data);
      auto oss_result = client->PutObject(
          opts_.bucket, object_key, content_stream);

      if (oss_result.isSuccess()) {
        result.success = true;
        result.object_key = object_key;
        result.error_code.clear();
        result.error_message.clear();
        break;
      }

      auto const& oss_error = oss_result.error();
      result.error_code = oss_error.Code();
      result.error_message = oss_error.Message();

      LOG(WARNING) << "[OssClient] PutObject failed, request_id=" << request_id
                   << " attempt=" << attempt << "/" << (max_retries + 1)
                   << " code=" << result.error_code
                   << " msg=" << result.error_message;

      if (!IsRetryable(result.error_code)) break;
      if (attempt > max_retries) {
        LOG(ERROR) << "[OssClient] All retries exhausted, request_id=" << request_id;
        break;
      }
    }

    auto end_ts = std::chrono::steady_clock::now();
    result.duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_ts - start_ts).count();

    if (result.success) {
      LOG(INFO) << "[OssClient] Upload success, request_id=" << request_id
                << " object_key=" << result.object_key
                << " duration_ms=" << result.duration_ms;
    } else {
      LOG(ERROR) << "[OssClient] Upload failed, request_id=" << request_id
                 << " code=" << result.error_code
                 << " msg=" << result.error_message
                 << " duration_ms=" << result.duration_ms;
    }

    if (callback) callback(result);
  });
}

void OssClient::UploadMultipartAsync(
    const std::string& image_data,
    const std::unordered_map<std::string, std::string>& metadata,
    UploadCallback callback,
    int64_t part_size) {
  upload_pool_.Submit([this, image_data, metadata, callback, part_size]() {
    auto start_ts = std::chrono::steady_clock::now();

    auto rid_it = metadata.find("request_id");
    std::string request_id = (rid_it != metadata.end()) ? rid_it->second : "unknown";

    auto* client = static_cast<AlibabaCloud::OSS::OssClient*>(oss_client_);
    if (!client) {
      LOG(ERROR) << "[OssClient][MP] OSS client is null, request_id=" << request_id;
      UploadResult result;
      result.success = false;
      result.error_code = "CLIENT_ERROR";
      result.error_message = "OSS client not initialized";
      auto end_ts = std::chrono::steady_clock::now();
      result.duration_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(end_ts - start_ts).count();
      if (callback) callback(result);
      return;
    }

    std::string object_key;
    auto ok_it = metadata.find("object_key");
    if (ok_it != metadata.end()) {
      object_key = ok_it->second;
    } else {
      object_key = request_id + ".jpg";
    }

    // 1. InitiateMultipartUpload
    AlibabaCloud::OSS::InitiateMultipartUploadRequest init_request(
        opts_.bucket, object_key);
    auto init_outcome = client->InitiateMultipartUpload(init_request);
    if (!init_outcome.isSuccess()) {
      auto const& err = init_outcome.error();
      LOG(ERROR) << "[OssClient][MP] InitiateMultipartUpload failed, "
                 << "request_id=" << request_id
                 << " code=" << err.Code() << " msg=" << err.Message();
      UploadResult result;
      result.success = false;
      result.error_code = err.Code();
      result.error_message = err.Message();
      auto end_ts = std::chrono::steady_clock::now();
      result.duration_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(end_ts - start_ts).count();
      if (callback) callback(result);
      return;
    }

    std::string upload_id = init_outcome.result().UploadId();
    VLOG(1) << "[OssClient][MP] Initiated, request_id=" << request_id
            << " upload_id=" << upload_id;

    // 2. 切分并上传各个分片
    int64_t data_size = static_cast<int64_t>(image_data.size());
    int64_t real_part_size = part_size;
    if (real_part_size <= 0) real_part_size = 1 * 1024 * 1024;

    int part_count = static_cast<int>((data_size + real_part_size - 1) / real_part_size);
    std::vector<AlibabaCloud::OSS::Part> parts;
    parts.reserve(part_count);

    UploadResult result;
    bool failed = false;

    for (int i = 0; i < part_count; ++i) {
      int64_t offset = static_cast<int64_t>(i) * real_part_size;
      int64_t this_part_size = std::min(real_part_size, data_size - offset);

      auto part_stream = std::make_shared<std::stringstream>(
          image_data.substr(offset, this_part_size));

      AlibabaCloud::OSS::UploadPartRequest part_request(
          opts_.bucket, object_key,
          i + 1, upload_id, part_stream);
      part_request.setContentLength(this_part_size);

      auto part_outcome = client->UploadPart(part_request);
      if (!part_outcome.isSuccess()) {
        auto const& err = part_outcome.error();
        LOG(ERROR) << "[OssClient][MP] UploadPart failed, request_id=" << request_id
                   << " part=" << (i + 1) << "/" << part_count
                   << " code=" << err.Code() << " msg=" << err.Message();
        // 出错时尝试中止分片上传
        AlibabaCloud::OSS::AbortMultipartUploadRequest abort_request(
            opts_.bucket, object_key, upload_id);
        client->AbortMultipartUpload(abort_request);
        result.success = false;
        result.error_code = err.Code();
        result.error_message = err.Message();
        failed = true;
        break;
      }

      AlibabaCloud::OSS::Part part(i + 1, part_outcome.result().ETag());
      parts.push_back(part);
    }

    if (failed) {
      auto end_ts = std::chrono::steady_clock::now();
      result.duration_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(end_ts - start_ts).count();
      if (callback) callback(result);
      return;
    }

    // 3. CompleteMultipartUpload
    AlibabaCloud::OSS::CompleteMultipartUploadRequest comp_request(
        opts_.bucket, object_key, parts, upload_id);
    auto comp_outcome = client->CompleteMultipartUpload(comp_request);
    if (!comp_outcome.isSuccess()) {
      auto const& err = comp_outcome.error();
      LOG(ERROR) << "[OssClient][MP] CompleteMultipartUpload failed, "
                 << "request_id=" << request_id
                 << " code=" << err.Code() << " msg=" << err.Message();
      result.success = false;
      result.error_code = err.Code();
      result.error_message = err.Message();
    } else {
      result.success = true;
      result.object_key = object_key;
      result.error_code.clear();
      result.error_message.clear();
    }

    auto end_ts = std::chrono::steady_clock::now();
    result.duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_ts - start_ts).count();

    if (result.success) {
      LOG(INFO) << "[OssClient][MP] Upload success, request_id=" << request_id
                << " object_key=" << result.object_key
                << " duration_ms=" << result.duration_ms;
    } else {
      LOG(ERROR) << "[OssClient][MP] Upload failed, request_id=" << request_id
                 << " code=" << result.error_code
                 << " msg=" << result.error_message
                 << " duration_ms=" << result.duration_ms;
    }

    if (callback) callback(result);
  });
}

}  // namespace gate
}  // namespace oss
}  // namespace mould
