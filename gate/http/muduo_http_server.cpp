#include "gate/http/muduo_http_server.hpp"

#include <muduo/base/Timestamp.h>
#include <muduo/net/Buffer.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>

namespace mould {
namespace gate {
namespace http {

// ============================================================
// 内部辅助
// ============================================================
namespace {
// 注意：request_id 由编排 IngestHandler 内部生成 (uuid_generator.hpp)，
// 格式为 gw-{timestamp}-{seq_hex}{random_hex}，故此处不再重复实现。
}  // anonymous namespace

// ============================================================
// TokenBucket
// ============================================================

MuduoHttpServer::TokenBucket::TokenBucket(double rate, double burst)
    : rate_(rate > 0.0 ? rate : 1.0),
      burst_size_(burst > 0.0 ? burst : 1.0),
      tokens_(burst_size_),
      last_refill_(std::chrono::steady_clock::now()) {}

bool MuduoHttpServer::TokenBucket::Consume() {
  std::lock_guard<std::mutex> lock(mutex_);
  auto now = std::chrono::steady_clock::now();
  auto elapsed =
      std::chrono::duration<double>(now - last_refill_).count();
  if (elapsed > 0.0) {
    tokens_ = std::min(burst_size_, tokens_ + elapsed * rate_);
    last_refill_ = now;
  }
  if (tokens_ >= 1.0) {
    tokens_ -= 1.0;
    return true;
  }
  return false;
}

void MuduoHttpServer::TokenBucket::Reset(double rate, double burst) {
  std::lock_guard<std::mutex> lock(mutex_);
  rate_ = (rate > 0.0) ? rate : 1.0;
  burst_size_ = (burst > 0.0) ? burst : 1.0;
  tokens_ = burst_size_;
  last_refill_ = std::chrono::steady_clock::now();
}

double MuduoHttpServer::TokenBucket::tokens() const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto now = std::chrono::steady_clock::now();
  auto elapsed =
      std::chrono::duration<double>(now - last_refill_).count();
  if (elapsed > 0.0) {
    return std::min(burst_size_, tokens_ + elapsed * rate_);
  }
  return tokens_;
}

// ============================================================
// HttpContext — 每连接 HTTP 解析状态机
// ============================================================

struct MuduoHttpServer::HttpContext {
  enum State : uint8_t {
    kRequestLine,  // 正在解析请求行
    kHeaders,      // 正在解析头部
    kBody,         // 正在累加请求体
    kComplete,     // 请求已完整接收，待处理
    kError         // 解析错误
  };

  State state = kRequestLine;
  std::string method;
  std::string path;
  std::unordered_map<std::string, std::string> headers;
  std::string body;
  size_t content_length = 0;

  void Reset() {
    state = kRequestLine;
    method.clear();
    path.clear();
    headers.clear();
    body.clear();
    content_length = 0;
  }
};

// ============================================================
// MuduoHttpServer
// ============================================================

MuduoHttpServer::MuduoHttpServer(Options opts, RequestCallback cb)
    : opts_(std::move(opts)), cb_(std::move(cb)) {}

MuduoHttpServer::~MuduoHttpServer() { DoCleanup(); }

bool MuduoHttpServer::Start() {
  if (started_.exchange(true)) {
    return false;  // 已启动，忽略
  }

  loop_ = loop_thread_.startLoop();

  muduo::net::InetAddress addr(opts_.listen_addr, opts_.port);
  server_ = std::make_unique<muduo::net::TcpServer>(loop_, addr,
                                                     "MuduoHttpServer");

  server_->setConnectionCallback(
      std::bind(&MuduoHttpServer::OnConnection, this,
                std::placeholders::_1));
  server_->setMessageCallback(
      std::bind(&MuduoHttpServer::OnMessage, this,
                std::placeholders::_1, std::placeholders::_2,
                std::placeholders::_3));

  if (opts_.io_thread_count > 0) {
    server_->setThreadNum(opts_.io_thread_count);
  }  // TcpServer::start() calls EventLoopThreadPool::start() which
  // asserts baseLoop_->assertInLoopThread(), so must run in the I/O thread.
  loop_->runInLoop([this] { server_->start(); });
  return true;
}

void MuduoHttpServer::DoCleanup() {
  if (!started_.exchange(false)) {
    return;
  }

  // 1. 在 EventLoop 线程中销毁 TcpServer（停止监听，关闭已接受连接）
  if (loop_) {
    muduo::net::TcpServer* raw = server_.release();
    loop_->runInLoop([raw]() { delete raw; });
    loop_->quit();
    loop_ = nullptr;
  }

  // 2. 等待活跃连接完成（带 5 秒超时）
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (active_connections_.load(std::memory_order_acquire) > 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

// ============================================================
// 连接回调
// ============================================================

void MuduoHttpServer::OnConnection(
    const muduo::net::TcpConnectionPtr& conn) {
  if (conn->connected()) {
    // 并发连接上限检查
    int current = active_connections_.load(std::memory_order_relaxed);
    if (current >= static_cast<int>(opts_.max_connections)) {
      SendErrorResponse(conn, 503, "Service Unavailable");
      conn->forceClose();
      return;
    }
    active_connections_.fetch_add(1, std::memory_order_relaxed);
    conn->setContext(std::make_shared<HttpContext>());
  } else {
    active_connections_.fetch_sub(1, std::memory_order_relaxed);
  }
}

// ============================================================
// 消息回调 — HTTP 请求解析
// ============================================================

void MuduoHttpServer::OnMessage(const muduo::net::TcpConnectionPtr& conn,
                                 muduo::net::Buffer* buf,
                                 muduo::Timestamp receiveTime) {
  // 获取连接上下文
  auto* ctx_shared =
      boost::any_cast<std::shared_ptr<HttpContext>>(conn->getMutableContext());
  if (!ctx_shared || !*ctx_shared) {
    conn->forceClose();
    return;
  }
  auto& ctx = **ctx_shared;

  // 新请求开始时检查速率限制
  std::string client_ip = conn->peerAddress().toIp();
  if (ctx.state == HttpContext::kRequestLine && !CheckRateLimit(client_ip)) {
    SendErrorResponse(conn, 429, "Too Many Requests",
                      "Retry-After: 1\r\n");
    ctx.state = HttpContext::kError;
    return;
  }

  // 状态机：逐字节消费 buffer，直到请求完整或等待更多数据
  while (buf->readableBytes() > 0 && ctx.state < HttpContext::kComplete) {
    switch (ctx.state) {
      // --------------------------------------------------------
      // 请求行: METHOD /path HTTP/1.1\r\n
      // --------------------------------------------------------
      case HttpContext::kRequestLine: {
        const char* crlf = buf->findCRLF();
        if (!crlf) {
          return;  // 数据不完整，等待更多
        }

        const char* start = buf->peek();
        const char* line_end = crlf;

        // 方法名
        const char* sp1 = static_cast<const char*>(
            memchr(start, ' ', static_cast<size_t>(line_end - start)));
        if (!sp1) {
          ctx.state = HttpContext::kError;
          SendErrorResponse(conn, 400, "Bad Request");
          return;
        }
        ctx.method.assign(start, static_cast<size_t>(sp1 - start));

        // 路径（含 query string）
        const char* sp2 = static_cast<const char*>(
            memchr(sp1 + 1, ' ',
                   static_cast<size_t>(line_end - (sp1 + 1))));
        if (!sp2) {
          ctx.state = HttpContext::kError;
          SendErrorResponse(conn, 400, "Bad Request");
          return;
        }
        ctx.path.assign(sp1 + 1, static_cast<size_t>(sp2 - (sp1 + 1)));

        buf->retrieveUntil(crlf + 2);
        ctx.state = HttpContext::kHeaders;
        break;
      }

      // --------------------------------------------------------
      // 头部: Key: Value\r\n  ...  \r\n
      // --------------------------------------------------------
      case HttpContext::kHeaders: {
        const char* crlf = buf->findCRLF();
        if (!crlf) {
          return;
        }

        if (crlf == buf->peek()) {
          // 空行：头部结束，准备进入请求体
          buf->retrieve(2);

          // 解析 Content-Length
          auto it = ctx.headers.find("content-length");
          if (it != ctx.headers.end()) {
            ctx.content_length =
                static_cast<size_t>(std::atoll(it->second.c_str()));
          }

          // 请求体超限检查
          if (ctx.content_length > opts_.max_body_size) {
            ctx.state = HttpContext::kError;
            SendErrorResponse(conn, 413, "Request Entity Too Large");
            return;
          }

          ctx.state = (ctx.content_length == 0) ? HttpContext::kComplete
                                                : HttpContext::kBody;
          break;
        }

        // 解析单行头部
        const char* colon = static_cast<const char*>(
            memchr(buf->peek(), ':', static_cast<size_t>(crlf - buf->peek())));
        if (colon) {
          std::string key(buf->peek(),
                          static_cast<size_t>(colon - buf->peek()));
          // 统一转为小写
          for (auto& c : key) {
            c = static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
          }
          // 跳过冒号后的空白
          const char* val_start = colon + 1;
          while (val_start < crlf &&
                 (*val_start == ' ' || *val_start == '\t')) {
            ++val_start;
          }
          std::string value(
              val_start, static_cast<size_t>(crlf - val_start));
          ctx.headers[key] = value;
        }

        buf->retrieveUntil(crlf + 2);
        break;
      }

      // --------------------------------------------------------
      // 请求体: Content-Length 字节
      // --------------------------------------------------------
      case HttpContext::kBody: {
        size_t available = buf->readableBytes();
        size_t needed = ctx.content_length - ctx.body.size();
        size_t to_read = std::min(available, needed);
        ctx.body.append(buf->peek(), to_read);
        buf->retrieve(to_read);
        if (ctx.body.size() >= ctx.content_length) {
          ctx.state = HttpContext::kComplete;
        } else {
          return;
        }
        break;
      }

      default:
        return;
    }
  }

  // 请求完整 → 处理
  if (ctx.state == HttpContext::kComplete) {
    if (!started_.load(std::memory_order_acquire)) {
      SendErrorResponse(conn, 503, "Service Unavailable");
      ctx.Reset();
      return;
    }
    ProcessRequest(conn, ctx.method, ctx.path, ctx.headers, ctx.body,
                   client_ip, receiveTime);
    ctx.Reset();
  } else if (ctx.state == HttpContext::kError) {
    conn->forceClose();
  }
}

// ============================================================
// 处理已解析的 HTTP 请求
// ============================================================

void MuduoHttpServer::ProcessRequest(
    const muduo::net::TcpConnectionPtr& conn, const std::string& method,
    const std::string& path,
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& body, const std::string& client_ip,
    muduo::Timestamp receiveTime) {
  // ----------------------------------------------------------
  // /health 端点 — 返回服务器及 handler 层健康指标
  // ----------------------------------------------------------
  if (path == "/health") {
    std::ostringstream json;
    int conn_count = active_connections_.load(std::memory_order_relaxed);
    if (health_cb_) {
      HealthMetrics ext = health_cb_();
      json << "{\"status\":\"ok\",\"active_connections\":" << conn_count
           << ",\"total_requests\":" << ext.total_requests
           << ",\"success_count\":" << ext.success_count
           << ",\"failure_count\":" << ext.failure_count
           << ",\"avg_latency_ms\":" << ext.avg_latency_ms
           << ",\"p99_latency_ms\":" << ext.p99_latency_ms
           << ",\"shm_rejected_count\":" << ext.shm_rejected_count
           << ",\"oss_failure_count\":" << ext.oss_failure_count
           << ",\"oss_retry_count\":" << ext.oss_retry_count
           << "}";
    } else {
      json << "{\"status\":\"ok\",\"active_connections\":" << conn_count
           << ",\"note\":\"health callback not registered\"}";
    }
    std::string json_body = json.str();
    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << json_body.size() << "\r\n"
        << "Connection: keep-alive\r\n"
        << "\r\n"
        << json_body;
    conn->send(oss.str().data(), static_cast<int>(oss.str().size()));
    return;
  }

  // ----------------------------------------------------------
  // 正常请求处理 — 使用异步回调模式
  // ----------------------------------------------------------
  // 构建 RequestView（request_id 由流水线内部生成，此处不设）
  RequestView view;
  view.client_ip = client_ip;
  view.method = method;
  view.path = path;
  view.headers = headers;   // 透传所有 HTTP 头部，供路由/流水线使用

  auto ct_it = headers.find("content-type");
  if (ct_it != headers.end()) {
    view.content_type = ct_it->second;
  }

  view.content_length = static_cast<int64_t>(body.size());
  view.arrival_timestamp_ms =
      receiveTime.microSecondsSinceEpoch() / 1000;

  // 原始请求体写入通用 body
  view.body = body;
  // view.image_data = body;  // 移除冗余拷贝，ingest_handler 使用 view.body

  // 回调 — 投递到编排队列/流水线，携带响应发送器
  // 流水线完成后通过 sender 异步发送 HTTP 响应，确保响应反映流水线真实结果
  if (cb_) {
    // TcpConnectionPtr 是 shared_ptr，lambda 捕获延长连接生命周期
    auto sender = [conn](int status_code,
                         const std::string& json_body,
                         const std::string& status_msg) {
      std::ostringstream oss;
      oss << "HTTP/1.1 " << status_code << " " << status_msg << "\r\n"
          << "Content-Type: application/json\r\n"
          << "Content-Length: " << json_body.size() << "\r\n"
          << "Connection: keep-alive\r\n"
          << "\r\n"
          << json_body;
      conn->send(oss.str().data(), static_cast<int>(oss.str().size()));
    };

    cb_(std::move(view), std::move(sender));
  }
  // 注意：此处不直接发送响应，由流水线完成后通过 sender 发送
}

// ============================================================
// 发送 HTTP 错误响应
// ============================================================

void MuduoHttpServer::SendErrorResponse(
    const muduo::net::TcpConnectionPtr& conn, int status_code,
    const std::string& status_msg, const std::string& extra_headers) {
  std::string resp = "HTTP/1.1 " + std::to_string(status_code) + " " +
                     status_msg +
                     "\r\n"
                     "Content-Length: 0\r\n"
                     "Connection: close\r\n";
  if (!extra_headers.empty()) {
    resp += extra_headers;
    if (resp.back() != '\n') {
      resp += "\r\n";
    }
  }
  resp += "\r\n";
  conn->send(resp.data(), static_cast<int>(resp.size()));
}

// ============================================================
// 速率限制检查
// ============================================================

bool MuduoHttpServer::CheckRateLimit(const std::string& client_ip) {
  std::lock_guard<std::mutex> lock(rate_mutex_);
  auto it = rate_limiters_.find(client_ip);
  if (it == rate_limiters_.end()) {
    auto bucket = std::make_unique<TokenBucket>(opts_.max_rate_per_conn,
                                                 opts_.max_rate_per_conn);
    it = rate_limiters_.emplace(client_ip, std::move(bucket)).first;
  }
  return it->second->Consume();
}

}  // namespace http
}  // namespace gate
}  // namespace mould
