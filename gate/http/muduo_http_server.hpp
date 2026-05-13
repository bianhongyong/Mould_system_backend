#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <muduo/net/EventLoop.h>
#include <muduo/net/EventLoopThread.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/TcpConnection.h>
#include <muduo/net/TcpServer.h>

#include "gate/http/request_view.hpp"

namespace mould {
namespace gate {
namespace http {

/// 健康检查指标，由外部 handler 层通过回调提供
struct HealthMetrics {
  uint64_t total_requests = 0;
  uint64_t success_count = 0;
  uint64_t failure_count = 0;
  double avg_latency_ms = 0.0;
  double p99_latency_ms = 0.0;
  uint64_t shm_rejected_count = 0;
  uint64_t oss_failure_count = 0;
  uint64_t oss_retry_count = 0;
};

/// 基于 muduo 的 HTTP 服务器。
///
/// 接收原始 HTTP 请求，转换为 RequestView 并通过回调传出。
/// 支持并发连接上限、请求体大小上限、单连接速率上限。
class MuduoHttpServer : public std::enable_shared_from_this<MuduoHttpServer> {
 public:
  /// 响应发送器：流水线完成后调用此函数发送 HTTP 响应
  /// 参数: status_code, json_body, status_message
  using ResponseSender = std::function<void(int, const std::string&, const std::string&)>;

  /// 请求到达回调：传入解析后的请求视图和响应发送器
  /// 回调负责执行流水线，完成后通过 ResponseSender 发送 HTTP 响应
  using RequestCallback = std::function<void(RequestView, ResponseSender)>;
  using HealthCallback = std::function<HealthMetrics()>;

  /// 服务器配置
  struct Options {
    std::string listen_addr = "0.0.0.0";
    uint16_t port = 8080;
    size_t max_connections = 10000;                    ///< 并发连接上限
    size_t max_body_size = 50 * 1024 * 1024;         ///< 单请求体上限（50 MB）
    double max_rate_per_conn = 10000.0;                 ///< 单连接速率上限（req/s）
    int io_thread_count = 4;                            ///< muduo I/O 线程数（0=单线程）
  };

  MuduoHttpServer(Options opts, RequestCallback cb);
  ~MuduoHttpServer();

  /// 启动服务器（非阻塞，在 EventLoopThread 中运行）
  bool Start();

  /// 优雅关闭：停止接受新连接，等待处理中请求完成，关闭 EventLoop
  void DoCleanup();

  const Options& GetOptions() const { return opts_; }

  /// 注册健康检查回调（可选）。注册后 /health 端点返回聚合指标。
  void SetHealthCallback(HealthCallback cb) { health_cb_ = std::move(cb); }

  // ------------------------------------------------------------------
  // TokenBucket — 令牌桶限速器（公开以便单元测试独立验证）
  // ------------------------------------------------------------------
  class TokenBucket {
   public:
    TokenBucket(double rate, double burst);
    bool Consume();
    void Reset(double rate, double burst);
    double tokens() const;

   private:
    double rate_;
    double burst_size_;
    double tokens_;
    std::chrono::steady_clock::time_point last_refill_;
    mutable std::mutex mutex_;
  };

 private:
  // 每连接的 HTTP 解析状态
  struct HttpContext;

  // --- muduo 网络回调 ---
  void OnConnection(const muduo::net::TcpConnectionPtr& conn);
  void OnMessage(const muduo::net::TcpConnectionPtr& conn,
                 muduo::net::Buffer* buf, muduo::Timestamp receiveTime);

  // --- 内部辅助 ---
  void ProcessRequest(const muduo::net::TcpConnectionPtr& conn,
                      const std::string& method, const std::string& path,
                      const std::unordered_map<std::string, std::string>& headers,
                      const std::string& body, const std::string& client_ip,
                      muduo::Timestamp receiveTime);
  void SendErrorResponse(const muduo::net::TcpConnectionPtr& conn,
                         int status_code, const std::string& status_msg,
                         const std::string& extra_headers = "");
  bool CheckRateLimit(const std::string& client_ip);

  Options opts_;
  RequestCallback cb_;
  HealthCallback health_cb_;

  muduo::net::EventLoopThread loop_thread_;
  std::unique_ptr<muduo::net::TcpServer> server_;
  muduo::net::EventLoop* loop_ = nullptr;

  std::atomic<int> active_connections_{0};
  std::atomic<bool> started_{false};

  mutable std::mutex rate_mutex_;
  std::unordered_map<std::string, std::unique_ptr<TokenBucket>> rate_limiters_;
};

}  // namespace http
}  // namespace gate
}  // namespace mould
