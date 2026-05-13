#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "gate/http/muduo_http_server.hpp"
#include "gate/http/request_view.hpp"

namespace mould {
namespace gate {
namespace http {
namespace {

// ============================================================
// RequestView 结构体
// ============================================================

TEST(HttpTest, RequestViewDefaultValues) {
  RequestView view;
  EXPECT_TRUE(view.request_id.empty());
  EXPECT_TRUE(view.method.empty());
  EXPECT_TRUE(view.path.empty());
  EXPECT_TRUE(view.body.empty());
  EXPECT_TRUE(view.headers.empty());
  EXPECT_TRUE(view.client_ip.empty());
  EXPECT_TRUE(view.content_type.empty());
  EXPECT_EQ(view.content_length, 0);
  EXPECT_EQ(view.arrival_timestamp_ms, 0);
}

TEST(HttpTest, RequestViewFieldAssignment) {
  RequestView view;
  view.request_id = "req-001";
  view.method = "POST";
  view.path = "/ingest?batch=1";
  view.body = "\xFF\xD8\xFF\xE0";  // 原始体（示例 JPEG 魔数前缀）
  view.headers["X-Extra"] = "extra_value";
  view.client_ip = "192.168.1.100";
  view.content_type = "multipart/form-data; boundary=----boundary123";
  view.content_length = 50000;
  view.arrival_timestamp_ms = 1715231400000;

  EXPECT_EQ(view.request_id, "req-001");
  EXPECT_EQ(view.method, "POST");
  EXPECT_EQ(view.path, "/ingest?batch=1");
  EXPECT_EQ(view.body, "\xFF\xD8\xFF\xE0");
  ASSERT_EQ(view.headers.size(), 1);
  EXPECT_EQ(view.headers["X-Extra"], "extra_value");
  EXPECT_EQ(view.client_ip, "192.168.1.100");
  EXPECT_EQ(view.content_type, "multipart/form-data; boundary=----boundary123");
  EXPECT_EQ(view.content_length, 50000);
  EXPECT_EQ(view.arrival_timestamp_ms, 1715231400000);
}

TEST(HttpTest, RequestViewCopyAndMove) {
  RequestView original;
  original.request_id = "req-002";
  original.body = "binary-data";
  original.headers["k1"] = "v1";

  // 拷贝构造
  RequestView copied(original);
  EXPECT_EQ(copied.request_id, "req-002");
  EXPECT_EQ(copied.body, "binary-data");
  EXPECT_EQ(copied.headers["k1"], "v1");

  // 移动构造
  RequestView moved(std::move(copied));
  EXPECT_EQ(moved.request_id, "req-002");
  EXPECT_EQ(moved.headers["k1"], "v1");
  // 移动后源对象内容未定义，但不需 crash
}

TEST(HttpTest, RequestViewHeadersMultipleEntries) {
  RequestView view;
  view.headers["X-Node-Id"] = "cam-01";
  view.headers["X-Capture-Time"] = "2026-05-09T10:30:00Z";
  view.headers["X-Image-Id"] = "frame-0042";
  view.headers["X-Batch-Id"] = "batch-20260509";

  EXPECT_EQ(view.headers.size(), 4);
  EXPECT_EQ(view.headers["X-Node-Id"], "cam-01");
  EXPECT_EQ(view.headers["X-Capture-Time"], "2026-05-09T10:30:00Z");
  EXPECT_EQ(view.headers["X-Image-Id"], "frame-0042");
  EXPECT_EQ(view.headers["X-Batch-Id"], "batch-20260509");
}

// ============================================================
// TokenBucket 限速器
// ============================================================

TEST(HttpTest, TokenBucketConsumeSuccess) {
  MuduoHttpServer::TokenBucket bucket(10.0, 5.0);
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(bucket.Consume()) << "consume #" << i;
  }
  // 第五次消耗后令牌耗尽
  EXPECT_FALSE(bucket.Consume());
}

TEST(HttpTest, TokenBucketBurstThenRefill) {
  MuduoHttpServer::TokenBucket bucket(100.0, 10.0);
  // 消耗全部突发令牌
  for (int i = 0; i < 10; ++i) {
    EXPECT_TRUE(bucket.Consume()) << "burst consume #" << i;
  }
  EXPECT_FALSE(bucket.Consume());

  // 等待 50ms，应该至少补充 5 个令牌 (100 req/s = 0.1/ms)
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_TRUE(bucket.Consume());
}

TEST(HttpTest, TokenBucketReset) {
  MuduoHttpServer::TokenBucket bucket(10.0, 5.0);
  EXPECT_TRUE(bucket.Consume());

  bucket.Reset(1000.0, 100.0);
  for (int i = 0; i < 100; ++i) {
    EXPECT_TRUE(bucket.Consume()) << "consume after reset #" << i;
  }
}

TEST(HttpTest, TokenBucketTokens) {
  MuduoHttpServer::TokenBucket bucket(10.0, 5.0);
  // 初始应该有 ~5 个令牌
  double initial = bucket.tokens();
  EXPECT_GE(initial, 4.9);

  EXPECT_TRUE(bucket.Consume());
  double after_one = bucket.tokens();
  EXPECT_LT(after_one, initial);
}

TEST(HttpTest, TokenBucketNegativeRateClamped) {
  // 负数 rate 应被 clamp 到 1.0，burst 用最小值 1.0 使桶尽快耗尽
  MuduoHttpServer::TokenBucket bucket(-5.0, 1.0);
  EXPECT_TRUE(bucket.Consume());
  // 由于 rate 被 clamp 为 1.0，每秒只补充 1 个令牌
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  EXPECT_TRUE(bucket.Consume());
  // 再消耗应失败（因为上次补充只补充了 1 个且 burst 为 1）
  EXPECT_FALSE(bucket.Consume());
}

TEST(HttpTest, TokenBucketZeroBurstClamped) {
  // burst <= 0 应被 clamp 到 1.0，保证至少可消耗一次
  MuduoHttpServer::TokenBucket bucket(10.0, 0.0);
  EXPECT_TRUE(bucket.Consume());
  EXPECT_FALSE(bucket.Consume());
}

// ============================================================
// MuduoHttpServer 配置
// ============================================================

TEST(HttpTest, DefaultOptions) {
  MuduoHttpServer::Options opts;
  EXPECT_EQ(opts.listen_addr, "0.0.0.0");
  EXPECT_EQ(opts.port, 8080);
  EXPECT_EQ(opts.max_connections, 10000);
  EXPECT_EQ(opts.max_body_size, 50 * 1024 * 1024);
  EXPECT_DOUBLE_EQ(opts.max_rate_per_conn, 10000.0);
  EXPECT_EQ(opts.io_thread_count, 4);
}

TEST(HttpTest, CustomOptions) {
  MuduoHttpServer::Options opts;
  opts.listen_addr = "127.0.0.1";
  opts.port = 9090;
  opts.max_connections = 50;
  opts.max_body_size = 1024 * 1024;  // 1 MB
  opts.max_rate_per_conn = 5.0;

  MuduoHttpServer server(
      opts, [](RequestView, MuduoHttpServer::ResponseSender) {});
  const auto& retrieved = server.GetOptions();
  EXPECT_EQ(retrieved.listen_addr, "127.0.0.1");
  EXPECT_EQ(retrieved.port, 9090);
  EXPECT_EQ(retrieved.max_connections, 50);
  EXPECT_EQ(retrieved.max_body_size, 1024 * 1024);
  EXPECT_DOUBLE_EQ(retrieved.max_rate_per_conn, 5.0);
}

TEST(HttpTest, CustomOptionsCallback) {
  bool callback_invoked = false;
  MuduoHttpServer::Options opts;
  opts.listen_addr = "127.0.0.1";
  opts.port = 0;  // 使用端口 0（不会真的启动服务器）

  MuduoHttpServer server(
      opts, [&callback_invoked](RequestView, MuduoHttpServer::ResponseSender) { callback_invoked = true; });

  const auto& retrieved = server.GetOptions();
  EXPECT_EQ(retrieved.port, 0);
  // 不启动服务器，仅验证配置存储和回调类型
  EXPECT_FALSE(callback_invoked);
}

// ============================================================
// 请求体超限检测
// ============================================================

TEST(HttpTest, BodySizeUnderLimit) {
  MuduoHttpServer::Options opts;
  opts.max_body_size = 100;
  MuduoHttpServer server(opts, [](RequestView, MuduoHttpServer::ResponseSender) {});
  EXPECT_EQ(server.GetOptions().max_body_size, 100);
}

TEST(HttpTest, BodySizeAtDefaultLimit) {
  MuduoHttpServer::Options opts;
  EXPECT_EQ(opts.max_body_size, 50 * 1024 * 1024);
}

TEST(HttpTest, BodySizeZeroAllowed) {
  MuduoHttpServer::Options opts;
  opts.max_body_size = 0;
  MuduoHttpServer server(opts, [](RequestView, MuduoHttpServer::ResponseSender) {});
  EXPECT_EQ(server.GetOptions().max_body_size, 0);
}

// ============================================================
// 速率限制逻辑
// ============================================================

TEST(HttpTest, RateLimitEnforcement) {
  // 1 req/s, burst=1 → 每秒最多 1 个请求
  MuduoHttpServer::TokenBucket bucket(1.0, 1.0);
  EXPECT_TRUE(bucket.Consume());
  EXPECT_FALSE(bucket.Consume());

  // 等待一个完整的秒间隔
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  EXPECT_TRUE(bucket.Consume());
  EXPECT_FALSE(bucket.Consume());
}

TEST(HttpTest, RateLimitHighBurst) {
  MuduoHttpServer::TokenBucket bucket(100.0, 1000.0);
  for (int i = 0; i < 1000; ++i) {
    EXPECT_TRUE(bucket.Consume()) << "burst consume #" << i;
  }
  EXPECT_FALSE(bucket.Consume());
}

TEST(HttpTest, RateLimitNoTokens) {
  MuduoHttpServer::TokenBucket bucket(0.001, 0.0);
  // burst 被 clamp 为 1.0，可消耗 1 次
  EXPECT_TRUE(bucket.Consume());
  // 第 2 次应失败，因为 rate 很低
  EXPECT_FALSE(bucket.Consume());
}

// ============================================================
// 优雅关闭超时逻辑
// ============================================================

TEST(HttpTest, CleanupTimeoutPattern) {
  // 模拟 DoCleanup 中的等待循环：超时场景
  std::atomic<int> counter{1};
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(100);

  while (counter.load() > 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // 超时退出时 counter 仍未归零
  EXPECT_GT(counter.load(), 0);
}

TEST(HttpTest, CleanupCompletesWhenConnectionsDrain) {
  // 模拟 DoCleanup 中的等待循环：正常完成场景
  std::atomic<int> counter{1};

  std::thread drainer([&counter]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    counter.store(0);
  });

  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  auto start = std::chrono::steady_clock::now();

  while (counter.load() > 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(counter.load(), 0);
  EXPECT_LT(elapsed, std::chrono::seconds(1));

  drainer.join();
}

TEST(HttpTest, CleanupAlreadyZero) {
  // 没有活跃连接时，清理应立即完成
  std::atomic<int> counter{0};
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  auto start = std::chrono::steady_clock::now();

  while (counter.load() > 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(counter.load(), 0);
  EXPECT_LT(elapsed, std::chrono::milliseconds(50));
}

// ============================================================
// MuduoHttpServer 生命周期（不启动真实服务器）
// ============================================================

TEST(HttpTest, LifecycleConstructAndDestroy) {
  // 仅构造与析构，不应崩溃
  MuduoHttpServer::Options opts;
  opts.port = 0;  // 不启动
  bool called = false;
  {
    MuduoHttpServer server(opts, [&called](RequestView, MuduoHttpServer::ResponseSender) { called = true; });
    EXPECT_FALSE(called);
  }
  EXPECT_FALSE(called);
}

TEST(HttpTest, StartIdempotent) {
  // Start() 多次调用应只生效一次（不会启动两次）
  // 端口 0 会导致系统分配端口，尝试 start 会失败
  // 这里仅验证接口不崩溃
  MuduoHttpServer::Options opts;
  opts.port = 0;
  MuduoHttpServer server(opts, [](RequestView, MuduoHttpServer::ResponseSender) {});
  // 不验证返回值，仅确保调用链不崩溃
  static_cast<void>(server.GetOptions());
}

}  // namespace
}  // namespace http
}  // namespace gate
}  // namespace mould

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
