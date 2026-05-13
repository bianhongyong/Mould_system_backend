#pragma once

#include <memory>
#include <string>

#include "gate/http/request_view.hpp"
#include "gate/handler/frame_payload_builder.hpp"
#include "gate/handler/ihandler.hpp"
#include "gate/handler/metrics_recorder.hpp"
#include "gate/handler/rate_limiter.hpp"
#include "gate/handler/request_validator.hpp"
#include "gate/handler/shm_frame_publisher.hpp"
#include "gate/oss/iblob_storage_uploader.hpp"
#include "gate/oss/upload_path_policy.hpp"

namespace mould {
namespace gate {
namespace handler {

/// HTTP 图片注入 handler。
///
/// 继承 IHandler 模板方法基类，实现图片场景的三阶段 hooks：
///   DoParse   — 限流 + 从 HTTP Headers 读取元数据 + 字段校验
///   DoProcess — 组装 ImageFrameIngress proto + SHM 发布
///   PostProcess — 异步 OSS 上传（不阻塞 HTTP 响应）
///
/// 请求协议：
///   - 图片二进制直接作为 HTTP Body
///   - 元数据通过 HTTP Headers 传递：
///       X-Node-Id: camera-01       （必填）
///       X-Capture-Time: 2026-...   （必填）
///       Content-Type: image/jpeg    （用于图片格式检测）
///       X-Mould-*:                   （额外元数据，写入 extra_metadata）
class IngestHandler : public IHandler {
public:
    /// 构造函数注入所有依赖
    IngestHandler(
        std::unique_ptr<RequestValidator> validator,
        std::unique_ptr<IFramePayloadBuilder> payload_builder,
        std::unique_ptr<IShmFramePublisher> shm_publisher,
        std::unique_ptr<oss::IBlobStorageUploader> oss_uploader,
        std::unique_ptr<IRateLimiter> rate_limiter,
        std::unique_ptr<IMetricsRecorder> metrics_recorder,
        std::unique_ptr<oss::IUploadPathPolicy> upload_path_policy);

protected:
    // ============================================================
    // IHandler hooks 实现
    // ============================================================

    ParseResult DoParse(http::RequestView& view) override;
    ProcessResult DoProcess(const ParseResult& input) override;
    void PostProcess(const ProcessResult& result) override;

private:
    std::unique_ptr<RequestValidator> validator_;
    std::unique_ptr<IFramePayloadBuilder> payload_builder_;
    std::unique_ptr<IShmFramePublisher> shm_publisher_;
    std::unique_ptr<oss::IBlobStorageUploader> oss_uploader_;
    std::unique_ptr<IRateLimiter> rate_limiter_;
    std::unique_ptr<IMetricsRecorder> metrics_recorder_;
    std::unique_ptr<oss::IUploadPathPolicy> upload_path_policy_;

    /// SHM 发布通道名称
    std::string shm_channel_name_ = "ImageFrameIngress";
};

}  // namespace handler
}  // namespace gate
}  // namespace mould
