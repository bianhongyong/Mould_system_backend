#include "gate/handler/ingest_handler.hpp"
#include <any>
#include <chrono>
#include <cstdint>
#include <unordered_map>

#include <glog/logging.h>
#include <utility>

namespace mould {
namespace gate {
namespace handler {

namespace {

struct FrameContext {
    std::string node_id;
    std::string capture_time;
    std::string image_data;
    std::string image_format;
    std::string image_filename;
    std::string request_id;
    std::unordered_map<std::string, std::string> fields;
};

/// 从 Content-Type 头映射图片格式名称
std::string MapImageFormat(const std::string& content_type_value) {
    if (content_type_value.find("image/jpeg") != std::string::npos ||
        content_type_value.find("image/jpg") != std::string::npos) {
        return "jpeg";
    }
    if (content_type_value.find("image/png") != std::string::npos) {
        return "png";
    }
    if (content_type_value.find("image/bmp") != std::string::npos ||
        content_type_value.find("image/x-ms-bmp") != std::string::npos) {
        return "bmp";
    }
    return "";
}

/// 通过 magic bytes 检测图片格式
std::string DetectImageFormat(const std::string& data) {
    if (data.size() >= 3 &&
        static_cast<uint8_t>(data[0]) == 0xFF &&
        static_cast<uint8_t>(data[1]) == 0xD8 &&
        static_cast<uint8_t>(data[2]) == 0xFF) {
        return "jpeg";
    }
    if (data.size() >= 4 &&
        static_cast<uint8_t>(data[0]) == 0x89 &&
        data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        return "png";
    }
    if (data.size() >= 2 &&
        static_cast<uint8_t>(data[0]) == 0x42 &&
        static_cast<uint8_t>(data[1]) == 0x4D) {
        return "bmp";
    }
    return "";
}

}  // namespace

IngestHandler::IngestHandler(
    std::unique_ptr<RequestValidator> validator,
    std::unique_ptr<IFramePayloadBuilder> payload_builder,
    std::unique_ptr<IShmFramePublisher> shm_publisher,
    std::unique_ptr<oss::IBlobStorageUploader> oss_uploader,
    std::unique_ptr<IRateLimiter> rate_limiter,
    std::unique_ptr<IMetricsRecorder> metrics_recorder,
    std::unique_ptr<oss::IUploadPathPolicy> upload_path_policy)
    : validator_(std::move(validator))
    , payload_builder_(std::move(payload_builder))
    , shm_publisher_(std::move(shm_publisher))
    , oss_uploader_(std::move(oss_uploader))
    , rate_limiter_(std::move(rate_limiter))
    , metrics_recorder_(std::move(metrics_recorder))
    , upload_path_policy_(std::move(upload_path_policy)) {}

// ============================================================
// IHandler::DoParse — 限流 + 从 Headers 提取元数据 + 校验
// ============================================================

IHandler::ParseResult IngestHandler::DoParse(
    http::RequestView& view) {

    ParseResult result;

    // ---- Stage 0: Rate Limiting ----
    if (!rate_limiter_->Acquire()) {
        RequestMetrics metrics;
        metrics.request_id = view.request_id;
        metrics.http_status_code = 429;
        metrics.latency_ms = 0;
        metrics.shm_publish_success = false;
        metrics.oss_result = "skipped";
        metrics_recorder_->RecordRequest(metrics);

        result.ok = false;
        result.status_code = 429;
        result.error_msg = "Rate limit exceeded";
        return result;
    }

    // ---- Stage 1: 从 HTTP Headers 读取元数据 ----
    auto& frame = result.ctx.emplace<FrameContext>();
    frame.request_id = view.request_id;

    // node_id (必填)
    auto node_it = view.headers.find("x-node-id");
    if (node_it != view.headers.end()) {
        frame.node_id = node_it->second;
    }

    // capture_time (必填)
    auto time_it = view.headers.find("x-capture-time");
    if (time_it != view.headers.end()) {
        frame.capture_time = time_it->second;
    }

    // 图片二进制来自 HTTP Body
    frame.image_data = std::move(view.body);

    // 图片格式: 优先从 Content-Type 头推断，再通过 magic bytes 兜底检测
    auto ct_it = view.headers.find("content-type");
    if (ct_it != view.headers.end()) {
        frame.image_format = MapImageFormat(ct_it->second);
    }
    if (frame.image_format.empty() && !frame.image_data.empty()) {
        frame.image_format = DetectImageFormat(frame.image_data);
    }

    // 额外元数据: X-Mould-* 前缀的自定义头（写入时去掉前缀）
    static const std::string kMetadataPrefix = "x-mould-";
    for (const auto& [key, value] : view.headers) {
        if (key.compare(0, kMetadataPrefix.size(), kMetadataPrefix) == 0) {
            frame.fields[key.substr(kMetadataPrefix.size())] = value;
        }
    }

    // ---- Stage 2: Validate ----
    ValidationResult vr = validator_->Validate(
        frame.node_id, frame.capture_time,
        frame.image_data, frame.image_format);
    if (!vr.valid) {
        RequestMetrics metrics;
        metrics.request_id = view.request_id;
        metrics.node_id = frame.node_id;
        metrics.http_status_code = vr.http_status_code;
        metrics.shm_publish_success = false;
        metrics.oss_result = "skipped";
        metrics_recorder_->RecordRequest(metrics);

        result.ok = false;
        result.status_code = vr.http_status_code;
        result.error_msg = vr.error_message;
        return result;
    }

    result.ok = true;
    result.status_code = 200;
    return result;
}

// ============================================================
// IHandler::DoProcess — 组装载荷 + SHM 发布
// ============================================================

IHandler::ProcessResult IngestHandler::DoProcess(
    const ParseResult& input) {

    // ---- Stage 3: Build Payload ----
    auto& frame = std::any_cast<const FrameContext&>(input.ctx);

    // 收集额外元数据（X-Mould-* 头去掉前缀后的键值对）
    std::unordered_map<std::string, std::string> extra_metadata;
    for (const auto& [key, val] : frame.fields) {
        extra_metadata[key] = val;
    }

    std::string payload = payload_builder_->BuildPayload(
        frame.request_id,
        frame.node_id, frame.capture_time,
        std::move(frame.image_data), frame.image_format,
        extra_metadata);

    // ---- Stage 4: Publish SHM ----
    ShmPublishResult shm_result = shm_publisher_->Publish(
        shm_channel_name_, payload);
    if (!shm_result.success) {
        // 由于旧版 Execute() 从外部使用 request_id，
        // 此处 metrics 记录时不传入 node_id（与旧行为一致）
        RequestMetrics metrics;
        metrics.request_id = "";
        metrics.http_status_code = 503;
        metrics.shm_publish_success = false;
        metrics.oss_result = "skipped";
        metrics_recorder_->RecordRequest(metrics);

        ProcessResult result;
        result.ok = false;
        result.status_code = 503;
        result.error_msg = shm_result.error_message;
        return result;
    }

    // 记录成功指标
    RequestMetrics metrics;
    metrics.node_id = frame.node_id;
    metrics.http_status_code = 200;
    metrics.shm_publish_success = true;
    metrics.oss_result = "enqueued";
    metrics_recorder_->RecordRequest(metrics);

    ProcessResult result;
    result.ok = true;
    result.status_code = 200;
    result.request_id = frame.request_id;
    // result.resource_id = shm_result.image_id;
    // 将 FrameContext 转发给 PostProcess
    result.ctx = input.ctx;
    return result;

}

// ============================================================
// IHandler::PostProcess — 异步 OSS 上传（不阻塞 HTTP 响应）
// ============================================================

void IngestHandler::PostProcess(
    const ProcessResult& result) {

    // ---- Stage 5: Enqueue OSS Upload (async, non-blocking) ----
    auto& frame = std::any_cast<const FrameContext&>(result.ctx);
    std::unordered_map<std::string, std::string> oss_metadata;
    oss_metadata["request_id"] = result.request_id;
    oss_metadata["image_id"] = result.resource_id;
    oss_metadata["node_id"] = frame.node_id;
    oss_metadata["image_filename"] = frame.image_filename;
    oss_metadata["image_format"] = frame.image_format;
    oss_metadata["capture_time"] = frame.capture_time;

    if (upload_path_policy_) {
        oss_metadata["object_key"] =
            upload_path_policy_->GeneratePath(oss_metadata);
    }

    oss_uploader_->UploadAsync(
        frame.image_data,
        oss_metadata,
        [request_id = result.request_id,
         image_id = result.resource_id](
            const oss::IBlobStorageUploader::UploadResult& upload_result) {
            if (!upload_result.success) {
                LOG(WARNING) << "OSS upload failed for request "
                            << request_id << ": " << upload_result.error_message;
            } else {
                LOG(INFO) << "OSS upload succeeded for request "
                         << request_id << ", image_id=" << image_id
                         << ", object_key=" << upload_result.object_key;
            }
        });
}

}  // namespace handler
}  // namespace gate
}  // namespace mould
