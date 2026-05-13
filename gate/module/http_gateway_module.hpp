#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/comm/include/module_base.hpp"
#include "common/comm/include/module_factory_registry.hpp"

#include "gate/http/muduo_http_server.hpp"
#include "gate/handler/ingest_handler.hpp"
#include "gate/handler/counter_handler.hpp"
#include "gate/handler/shm_frame_publisher.hpp"
#include "gate/handler/rate_limiter.hpp"
#include "gate/handler/metrics_recorder.hpp"
#include "gate/router_dispatcher.hpp"
#include "gate/oss/oss_client.hpp"
#include "gate/oss/upload_path_policy.hpp"

namespace mould {
namespace gate {
namespace module {

/// HTTP 网关模块。
///
/// 继承 ModuleBase，使用 RouterDispatcher 将 HTTP 请求分发到不同 handler。
/// 当前注册的默认路由:
///   POST /api/v1/frames/upload → IngestHandler（图片上传）
///
/// 扩展方式：在 DoInit() 中调用 router_->RegisterRoute() 添加新路由。
class HttpGatewayModule : public comm::ModuleBase {
 public:
  explicit HttpGatewayModule(const comm::ModuleFactoryConfig& config);
  ~HttpGatewayModule() override;

  bool DoInit() override;
  bool SetupSubscriptions() override;  // 网关只生产不消费

  // ============================================================
  // SHM 发布适配器（公开以便单元测试）
  // ============================================================
  class ModuleShmPublisherAdapter : public handler::IShmFramePublisher {
   public:
    using PublishFn =
        std::function<absl::StatusOr<std::uint64_t>(
            const std::string&, comm::ByteBuffer)>;

    explicit ModuleShmPublisherAdapter(PublishFn publish_fn);

    handler::ShmPublishResult Publish(
        const std::string& channel_name,
        const std::string& payload) override;

   private:
    PublishFn publish_fn_;
  };

 protected:
  // ============================================================
  // 网关配置（protected 以允许单元测试访问）
  // ============================================================
  struct GatewayConfig {
    std::string listen_addr = "0.0.0.0";
    int port = 8080;
    int max_connections = 10000;
    int max_body_size = 50 * 1024 * 1024;  // 50MB
    double max_rate_per_conn = 10000.0;
    double qps_limit = 10000.0;
    int io_thread_count = 4;
    int oss_retry_count = 3;
    int timeout_ms = 5000;
  };

  GatewayConfig ParseConfig();

  // 工厂方法：创建图片流水线
  std::unique_ptr<handler::IngestHandler> CreateFrameHandler();

  std::unique_ptr<RouterDispatcher> router_;
  std::unique_ptr<http::MuduoHttpServer> http_server_;
  std::unique_ptr<handler::LoggingMetricsRecorder> metrics_;

  /// 持有已注册 handler 的所有权（RouterDispatcher 持有裸指针）
  std::vector<std::unique_ptr<handler::IHandler>> owned_handlers_;

  /// 计数器 handler 裸指针（供压测重置等操作）
  handler::CounterHandler* counter_ptr_ = nullptr;

  GatewayConfig config_;
};

}  // namespace module
}  // namespace gate
}  // namespace mould
