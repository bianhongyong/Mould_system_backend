#include "gate/module/http_gateway_module.hpp"

#include <cstdlib>

#include <glog/logging.h>

#include "gate/handler/uuid_generator.hpp"
#include "gate/gflags/gateway_module_gflags.hpp"
#include "gate/handler/counter_handler.hpp"

namespace mould {
namespace gate {
namespace module {

// ============================================================
// ModuleShmPublisherAdapter
// ============================================================

HttpGatewayModule::ModuleShmPublisherAdapter::ModuleShmPublisherAdapter(
    PublishFn publish_fn)
    : publish_fn_(std::move(publish_fn)) {}

handler::ShmPublishResult
HttpGatewayModule::ModuleShmPublisherAdapter::Publish(
    const std::string& channel_name,
    const std::string& payload) {
  handler::ShmPublishResult result;
  comm::ByteBuffer buf(payload.begin(), payload.end());
  auto status = publish_fn_(channel_name, std::move(buf));
  if (status.ok()) {
    result.success = true;
  } else {
    result.success = false;
    result.error_type = handler::ShmPublishResult::ErrorType::kOther;
    result.error_message = std::string(status.status().message());
  }
  return result;
}

// ============================================================
// HttpGatewayModule
// ============================================================

HttpGatewayModule::HttpGatewayModule(
    const comm::ModuleFactoryConfig& config)
    : comm::ModuleBase(config.module_name, config.runtime_context) {}

HttpGatewayModule::~HttpGatewayModule() = default;

bool HttpGatewayModule::DoInit() {
  config_ = ParseConfig();

  // ---- 创建指标记录器 ----
  metrics_ = std::make_unique<handler::LoggingMetricsRecorder>();

  // ---- 创建路由器 ----
  router_ = std::make_unique<RouterDispatcher>();

  // ---- 注册路由: 图片上传 POST /api/v1/frames/upload ----
  {
    auto frame_handler = CreateFrameHandler();
    router_->RegisterRoute("POST", "/api/v1/frames/upload",
                           frame_handler.get());
    owned_handlers_.push_back(std::move(frame_handler));
  }

  // ---- 注册路由: 计数器 GET/POST /api/v1/counter ----
  {
    auto counter_handler = std::make_unique<handler::CounterHandler>();
    router_->RegisterRoute("GET", "/api/v1/counter",
                           counter_handler.get());
    router_->RegisterRoute("POST", "/api/v1/counter",
                           counter_handler.get());
    // 暴露计数器指针，压测时可 Reset
    counter_ptr_ = counter_handler.get();
    owned_handlers_.push_back(std::move(counter_handler));
  }

  // ---- 注册兜底 404 ----
  router_->SetFallback(
      [](http::RequestView& view) -> handler::HandlerResult {
        handler::HandlerResult result;
        result.success = false;
        result.http_status_code = 404;
        result.error_message = "Not Found";
        result.request_id = view.request_id;
        return result;
      });

  // ---- 创建 muduo HTTP 服务器 ----
  http::MuduoHttpServer::Options http_opts;
  http_opts.listen_addr = config_.listen_addr;
  http_opts.port = static_cast<uint16_t>(config_.port);
  http_opts.max_connections = config_.max_connections;
  http_opts.max_body_size = static_cast<size_t>(config_.max_body_size);
  http_opts.max_rate_per_conn = config_.max_rate_per_conn;
  http_opts.io_thread_count = config_.io_thread_count;

  http_server_ = std::make_unique<http::MuduoHttpServer>(
      std::move(http_opts),
      [this](http::RequestView view,
             http::MuduoHttpServer::ResponseSender send) {
        // 生成请求 ID（路由层后续可统一在 Dispatch 中生成）
        if (view.request_id.empty()) {
          view.request_id = handler::GenerateRequestId();
        }

        // 路由器分发请求到对应流水线
        auto result = router_->Dispatch(view);

        // 统一构建 JSON 响应
        int code = result.http_status_code;
        if (code == 200) {
          LOG(INFO) << "[HttpResponse 200] request_id=" << result.request_id
                    << " method=" << view.method << " path=" << view.path;
        }
        std::string json_body;
        if (code == 200) {
          json_body = R"({"request_id":")" + result.request_id +
                      R"(","image_id":")" + result.resource_id +
                      R"(","message":"ok"})";
        } else {
          json_body = R"({"request_id":")" + result.request_id +
                      R"(","error":")" + std::to_string(code) +
                      R"(","message":")" + result.error_message +
                      R"("})";
        }
        std::string msg = (code == 200) ? "OK" : "Error";
        send(code, json_body, msg);

        // 记录指标
        handler::RequestMetrics metrics;
        metrics.request_id = result.request_id;
        metrics.http_status_code = result.http_status_code;
        metrics.shm_publish_success = result.success;
        metrics.oss_result = result.success ? "enqueued" : "skipped";
        metrics_->RecordRequest(metrics);
      });

  // 注册健康检查回调
  http_server_->SetHealthCallback(
      [this]() -> http::HealthMetrics {
        http::HealthMetrics m;
        m.total_requests = metrics_->TotalRequests();
        m.success_count = metrics_->SuccessRequests();
        m.failure_count = metrics_->FailedRequests();
        m.avg_latency_ms = metrics_->AvgLatencyMs();
        return m;
      });

  if (!http_server_->Start()) {
    LOG(ERROR) << "Failed to start HTTP server on "
               << config_.listen_addr << ":" << config_.port;
    return false;
  }

  LOG(INFO) << "HttpGatewayModule listening on "
            << config_.listen_addr << ":" << config_.port;
  return true;
}

bool HttpGatewayModule::SetupSubscriptions() {
  // 网关只生产（向 SHM 发布帧数据），不消费任何通道
  return true;
}

// ============================================================
// 工厂方法：创建图片上传流水线
// ============================================================

std::unique_ptr<handler::IngestHandler>
HttpGatewayModule::CreateFrameHandler() {

  auto validator = std::make_unique<handler::RequestValidator>();
  auto payload_builder = std::make_unique<handler::ImageFramePayloadBuilder>();

  // 创建 SHM 发布适配器，封装 ModuleBase::PublishWithStatus
  auto shm_publisher = std::make_unique<ModuleShmPublisherAdapter>(
      [this](const std::string& channel, comm::ByteBuffer payload) {
        return PublishWithStatus(channel, std::move(payload));
      });

  auto oss_uploader = std::make_unique<oss::OssClient>(
      oss::OssClient::Options::FromEnv());
  auto path_policy = std::make_unique<oss::DefaultUploadPathPolicy>();

  auto rate_limiter = std::make_unique<handler::TokenBucketRateLimiter>(
      config_.qps_limit, config_.qps_limit);

  return std::make_unique<handler::IngestHandler>(
      std::move(validator),
      std::move(payload_builder),
      std::move(shm_publisher),
      std::move(oss_uploader),
      std::move(rate_limiter),
      std::make_unique<handler::LoggingMetricsRecorder>(),
      std::move(path_policy));
}

// ============================================================
// 配置解析
// ============================================================

HttpGatewayModule::GatewayConfig HttpGatewayModule::ParseConfig() {
  GatewayConfig cfg;

  cfg.listen_addr = FLAGS_gateway_listen_addr;
  cfg.port = FLAGS_gateway_port;
  if (cfg.port <= 0 || cfg.port > 65535) cfg.port = 8080;

  cfg.max_connections = FLAGS_gateway_max_connections;
  if (cfg.max_connections <= 0) cfg.max_connections = 10000;

  cfg.max_body_size = FLAGS_gateway_max_body_size;

  cfg.max_rate_per_conn = FLAGS_gateway_max_rate;
  if (cfg.max_rate_per_conn <= 0.0) cfg.max_rate_per_conn = 10000.0;

  cfg.qps_limit = FLAGS_gateway_qps_limit;
  if (cfg.qps_limit <= 0.0) cfg.qps_limit = 10000.0;

  cfg.io_thread_count = FLAGS_gateway_io_threads;
  if (cfg.io_thread_count < 0) cfg.io_thread_count = 4;

  cfg.timeout_ms = FLAGS_gateway_timeout_ms;
  if (cfg.timeout_ms <= 0) cfg.timeout_ms = 5000;

  cfg.oss_retry_count = FLAGS_gateway_oss_retry;
  if (cfg.oss_retry_count < 0) cfg.oss_retry_count = 3;

  return cfg;
}

// ============================================================
// 模块注册
// ============================================================
REGISTER_MOULD_MODULE_AS("HttpGatewayModule", HttpGatewayModule);

}  // namespace module
}  // namespace gate
}  // namespace mould
