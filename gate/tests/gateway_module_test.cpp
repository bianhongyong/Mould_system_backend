#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <absl/status/statusor.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gate/gflags/gateway_module_gflags.hpp"

#include "common/comm/include/module_base.hpp"
#include "common/comm/include/module_factory_registry.hpp"
#include "common/comm/include/interfaces.hpp"

#include "gate/module/http_gateway_module.hpp"
#include "gate/handler/shm_frame_publisher.hpp"
#include "gate/handler/ingest_handler.hpp"
#include "gate/handler/metrics_recorder.hpp"

namespace mould {
namespace gate {
namespace module {
namespace {

using namespace ::testing;

// ============================================================
// 线程安全的 Mock SHM 发布器（用于多线程安全测试）
// ============================================================

class ThreadSafeFakeShmFramePublisher
    : public handler::IShmFramePublisher {
 public:
  handler::ShmPublishResult Publish(
      const std::string& channel_name,
      const std::string& payload) override {
    std::lock_guard<std::mutex> lock(mutex_);
    published_count_++;
    last_channel_ = channel_name;
    last_payload_ = payload;
    return result_to_return_;
  }

  int published_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return published_count_;
  }

  std::string last_channel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_channel_;
  }

  void SetResult(const handler::ShmPublishResult& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    result_to_return_ = result;
  }

 private:
  mutable std::mutex mutex_;
  int published_count_ = 0;
  std::string last_channel_;
  std::string last_payload_;
  handler::ShmPublishResult result_to_return_;
};

// ============================================================
// ModuleShmPublisherAdapter 测试
// ============================================================

TEST(GatewayModuleAdapterTest, PublishSuccess) {
  HttpGatewayModule::ModuleShmPublisherAdapter adapter(
      [](const std::string& /*channel*/,
         comm::ByteBuffer /*payload*/) -> absl::StatusOr<std::uint64_t> {
        return 42U;
      });

  auto result = adapter.Publish("test_channel", "hello_payload");
  EXPECT_TRUE(result.success);
}

TEST(GatewayModuleAdapterTest, PublishFailure) {
  HttpGatewayModule::ModuleShmPublisherAdapter adapter(
      [](const std::string& /*channel*/,
         comm::ByteBuffer /*payload*/) -> absl::StatusOr<std::uint64_t> {
        return absl::UnknownError("simulated publish failure");
      });

  auto result = adapter.Publish("test_channel", "payload");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_type,
            handler::ShmPublishResult::ErrorType::kOther);
  EXPECT_FALSE(result.error_message.empty());
}

// ============================================================
// 多线程 Publish 安全验证（5.3）
// ============================================================

TEST(GatewayModuleAdapterTest, MultiThreadedPublishSafety) {
  ThreadSafeFakeShmFramePublisher publisher;
  handler::ShmPublishResult ok_result;
  ok_result.success = true;
  ok_result.image_id = "thread-safe-img-";
  publisher.SetResult(ok_result);

  constexpr int kThreads = 8;
  constexpr int kPublishesPerThread = 1000;
  std::atomic<int> total_success{0};

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&]() {
      for (int i = 0; i < kPublishesPerThread; ++i) {
        auto result = publisher.Publish(
            "frame_ingress",
            "payload_" + std::to_string(i));
        if (result.success) {
          total_success.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(total_success.load(), kThreads * kPublishesPerThread);
  EXPECT_EQ(publisher.published_count(), kThreads * kPublishesPerThread);
}

/// 使用 ModuleShmPublisherAdapter 的多线程发布安全测试
TEST(GatewayModuleAdapterTest, AdapterMultiThreadedPublishSafety) {
  // 创建一个线程安全的计数器
  struct {
    std::mutex mutex;
    int call_count = 0;
    std::string last_channel;
  } state;

  HttpGatewayModule::ModuleShmPublisherAdapter adapter(
      [&state](const std::string& channel,
               comm::ByteBuffer /*payload*/)
          -> absl::StatusOr<std::uint64_t> {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.call_count++;
        state.last_channel = channel;
        return static_cast<std::uint64_t>(state.call_count);
      });

  constexpr int kThreads = 8;
  constexpr int kCallsPerThread = 500;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&adapter]() {
      for (int i = 0; i < kCallsPerThread; ++i) {
        adapter.Publish("frame_ingress",
                        "payload_adaptor_test_" + std::to_string(i));
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  std::lock_guard<std::mutex> lock(state.mutex);
  EXPECT_EQ(state.call_count, kThreads * kCallsPerThread);
}

// ============================================================
// 工作队列测试
// ============================================================

TEST(GatewayModuleWorkerTest, EnqueueAndProcess) {
  // 模拟模块的工作队列模式
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<int> work_queue;
  std::atomic<bool> running{true};
  std::atomic<int> processed_count{0};

  // 工作线程
  std::thread worker([&]() {
    while (running.load(std::memory_order_acquire)) {
      std::vector<int> batch;
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::milliseconds(50),
                    [&] {
                      return !work_queue.empty() ||
                             !running.load(std::memory_order_acquire);
                    });
        if (!running.load(std::memory_order_acquire) &&
            work_queue.empty()) {
          break;
        }
        if (work_queue.empty()) continue;
        batch.swap(work_queue);
      }
      for (int item : batch) {
        // 模拟处理
        processed_count.fetch_add(item, std::memory_order_relaxed);
      }
    }
  });

  // 生产者：入队多个工作项
  constexpr int kItems = 100;
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (int i = 0; i < kItems; ++i) {
      work_queue.push_back(1);
    }
  }
  cv.notify_one();

  // 等待处理完成
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  running = false;
  cv.notify_all();
  worker.join();

  EXPECT_EQ(processed_count.load(), kItems);
}

TEST(GatewayModuleWorkerTest, MultiThreadedEnqueueDequeue) {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<int> work_queue;
  std::atomic<bool> running{true};
  std::atomic<int> total_processed{0};

  // 消费者线程
  std::thread consumer([&]() {
    while (running.load(std::memory_order_acquire)) {
      std::vector<int> batch;
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::milliseconds(100),
                    [&] {
                      return !work_queue.empty() ||
                             !running.load(std::memory_order_acquire);
                      });
        if (!running.load(std::memory_order_acquire) &&
            work_queue.empty()) {
          break;
        }
        if (work_queue.empty()) continue;
        batch.swap(work_queue);
      }
      for (int v : batch) {
        total_processed.fetch_add(v, std::memory_order_relaxed);
      }
    }
  });

  // 多个生产者线程
  constexpr int kProducers = 4;
  constexpr int kItemsPerProducer = 250;
  std::vector<std::thread> producers;
  producers.reserve(kProducers);

  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&]() {
      for (int i = 0; i < kItemsPerProducer; ++i) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          work_queue.push_back(1);
        }
        cv.notify_one();
      }
    });
  }

  for (auto& p : producers) {
    p.join();
  }

  // 给消费者足够的时间处理
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  running = false;
  cv.notify_all();
  consumer.join();

  EXPECT_EQ(total_processed.load(), kProducers * kItemsPerProducer);
}

// ============================================================
// HttpGatewayModule 生命周期测试
// ============================================================

TEST(GatewayModuleLifecycleTest, ConstructorWorks) {
  // 使用最小配置构造模块，验证构造不会崩溃
  comm::ModuleFactoryConfig config;
  config.module_name = "HttpGatewayModule";

  (void)config;  // suppress -Wunused-variable for this test
  EXPECT_NO_THROW({
    auto module = std::make_unique<HttpGatewayModule>(config);
    EXPECT_NE(module, nullptr);
  });
}

TEST(GatewayModuleLifecycleTest, SetupSubscriptionsReturnsTrue) {
  // 网关不消费任何通道，验证 SetupSubscriptions 返回 true
  comm::ModuleFactoryConfig config;
  config.module_name = "HttpGatewayModule";

  class TestHttpGatewayModule : public HttpGatewayModule {
   public:
    using HttpGatewayModule::HttpGatewayModule;
    using HttpGatewayModule::SetupSubscriptions;
  };

  TestHttpGatewayModule module(config);
  EXPECT_TRUE(module.SetupSubscriptions());
}

TEST(GatewayModuleLifecycleTest, ModuleIsRegistered) {
  // 验证模块已通过 REGISTER_MOULD_MODULE_AS 注册
  auto registered = comm::ModuleFactoryRegistry::Instance()
                        .RegisteredNames();
  EXPECT_NE(registered.find("HttpGatewayModule"),
            registered.end());
}

TEST(GatewayModuleLifecycleTest, ModuleFactoryCreatesInstance) {
  std::string error;
  comm::ModuleFactoryConfig factory_config;
  factory_config.module_name = "HttpGatewayModule";
  auto instance =
      comm::ModuleFactoryRegistry::Instance().Create(
          "HttpGatewayModule", factory_config, &error);
  ASSERT_NE(instance, nullptr) << error;
  EXPECT_TRUE(error.empty());
}

// ============================================================
// 多线程环境下 ModuleFactoryRegistry 并发安全测试
// ============================================================
//
// 注意: MuduoHttpServer 的端口校验/冲突测试在 http_test 中覆盖，
// 网关模块层因 EventLoop 线程亲缘性约束不在单元测试中验证。
// ============================================================

TEST(GatewayModuleLifecycleTest, ConcurrentFactoryAccess) {
  constexpr int kThreads = 4;
  std::vector<std::thread> threads;
  std::atomic<int> success_count{0};

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&]() {
      for (int i = 0; i < 100; ++i) {
        std::string error;
        comm::ModuleFactoryConfig factory_config;
        factory_config.module_name = "HttpGatewayModule";
        auto instance =
            comm::ModuleFactoryRegistry::Instance().Create(
                "HttpGatewayModule", factory_config, &error);
        if (instance) {
          success_count.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(success_count.load(), kThreads * 100);
}

// ============================================================
// GFLAGS 配置测试（替换原有的环境变量配置测试）
// ============================================================

class GatewayConfigTest : public ::testing::Test {
 protected:
  // 暴露 ParseConfig 以供测试
  class TestModule : public HttpGatewayModule {
   public:
    using HttpGatewayModule::HttpGatewayModule;
    using HttpGatewayModule::ParseConfig;
  };

  void SetUp() override {
    saved_.listen_addr = FLAGS_gateway_listen_addr;
    saved_.port = FLAGS_gateway_port;
    saved_.max_connections = FLAGS_gateway_max_connections;
    saved_.max_body_size = FLAGS_gateway_max_body_size;
    saved_.max_rate = FLAGS_gateway_max_rate;
    saved_.qps_limit = FLAGS_gateway_qps_limit;
    saved_.io_threads = FLAGS_gateway_io_threads;
    saved_.timeout_ms = FLAGS_gateway_timeout_ms;
    saved_.oss_retry = FLAGS_gateway_oss_retry;
  }

  void TearDown() override {
    FLAGS_gateway_listen_addr = saved_.listen_addr;
    FLAGS_gateway_port = saved_.port;
    FLAGS_gateway_max_connections = saved_.max_connections;
    FLAGS_gateway_max_body_size = saved_.max_body_size;
    FLAGS_gateway_max_rate = saved_.max_rate;
    FLAGS_gateway_qps_limit = saved_.qps_limit;
    FLAGS_gateway_io_threads = saved_.io_threads;
    FLAGS_gateway_timeout_ms = saved_.timeout_ms;
    FLAGS_gateway_oss_retry = saved_.oss_retry;
  }

  static comm::ModuleFactoryConfig MakeConfig() {
    comm::ModuleFactoryConfig config;
    config.module_name = "HttpGatewayModule";
    return config;
  }

 private:
  struct SavedFlags {
    std::string listen_addr;
    int32_t port;
    int32_t max_connections;
    int32_t max_body_size;
    double max_rate;
    double qps_limit;
    int32_t io_threads;
    int32_t timeout_ms;
    int32_t oss_retry;
  } saved_;
};

TEST_F(GatewayConfigTest, DefaultValues) {
  TestModule module(MakeConfig());
  auto cfg = module.ParseConfig();

  EXPECT_EQ(cfg.listen_addr, "0.0.0.0");
  EXPECT_EQ(cfg.port, 8080);
  EXPECT_EQ(cfg.max_connections, 10000);
  EXPECT_EQ(cfg.max_body_size, 52428800);
  EXPECT_EQ(cfg.max_rate_per_conn, 10000.0);
  EXPECT_EQ(cfg.qps_limit, 10000.0);
  EXPECT_EQ(cfg.io_thread_count, 4);
  EXPECT_EQ(cfg.timeout_ms, 5000);
  EXPECT_EQ(cfg.oss_retry_count, 3);
}

TEST_F(GatewayConfigTest, OverridePort) {
  FLAGS_gateway_port = 9090;
  TestModule module(MakeConfig());
  auto cfg = module.ParseConfig();
  EXPECT_EQ(cfg.port, 9090);
}

TEST_F(GatewayConfigTest, OverrideListenAddr) {
  FLAGS_gateway_listen_addr = "127.0.0.1";
  TestModule module(MakeConfig());
  auto cfg = module.ParseConfig();
  EXPECT_EQ(cfg.listen_addr, "127.0.0.1");
}

TEST_F(GatewayConfigTest, InvalidPortClampedToDefault) {
  FLAGS_gateway_port = 99999;
  TestModule module(MakeConfig());
  auto cfg = module.ParseConfig();
  EXPECT_EQ(cfg.port, 8080);
}

TEST_F(GatewayConfigTest, NegativeQpsClampedToDefault) {
  FLAGS_gateway_qps_limit = -1.0;
  TestModule module(MakeConfig());
  auto cfg = module.ParseConfig();
  EXPECT_EQ(cfg.qps_limit, 10000.0);
}

TEST_F(GatewayConfigTest, OverrideQpsLimit) {
  FLAGS_gateway_qps_limit = 5000.0;
  TestModule module(MakeConfig());
  auto cfg = module.ParseConfig();
  EXPECT_EQ(cfg.qps_limit, 5000.0);
}

TEST_F(GatewayConfigTest, OverrideMaxConnections) {
  FLAGS_gateway_max_connections = 20000;
  TestModule module(MakeConfig());
  auto cfg = module.ParseConfig();
  EXPECT_EQ(cfg.max_connections, 20000);
}

TEST_F(GatewayConfigTest, OverrideMaxBodySize) {
  FLAGS_gateway_max_body_size = 1048576;
  TestModule module(MakeConfig());
  auto cfg = module.ParseConfig();
  EXPECT_EQ(cfg.max_body_size, 1048576);
}

TEST_F(GatewayConfigTest, OverrideTimeoutMs) {
  FLAGS_gateway_timeout_ms = 10000;
  TestModule module(MakeConfig());
  auto cfg = module.ParseConfig();
  EXPECT_EQ(cfg.timeout_ms, 10000);
}

TEST_F(GatewayConfigTest, OverrideOssRetry) {
  FLAGS_gateway_oss_retry = 5;
  TestModule module(MakeConfig());
  auto cfg = module.ParseConfig();
  EXPECT_EQ(cfg.oss_retry_count, 5);
}

}  // namespace
}  // namespace module
}  // namespace gate
}  // namespace mould

// ============================================================
// 测试主函数
// ============================================================

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
