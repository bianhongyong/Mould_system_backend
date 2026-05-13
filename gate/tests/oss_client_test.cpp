#include <gtest/gtest.h>

#include <cstdlib>
#include <future>
#include <string>
#include <unordered_map>

#include "oss/iblob_storage_uploader.hpp"
#include "oss/oss_client.hpp"

namespace {

using mould::gate::oss::IBlobStorageUploader;
using mould::gate::oss::OssClient;

// ============================================================
// IsRetryable 错误码判断测试
// ============================================================

TEST(OssClientTest, IsRetryableReturnsTrueForRetryableCodes) {
  EXPECT_TRUE(OssClient::IsRetryable("RequestTimeout"));
  EXPECT_TRUE(OssClient::IsRetryable("SlowDown"));
  EXPECT_TRUE(OssClient::IsRetryable("ServiceUnavailable"));
  EXPECT_TRUE(OssClient::IsRetryable("InternalError"));
  EXPECT_TRUE(OssClient::IsRetryable("SocketException"));
  EXPECT_TRUE(OssClient::IsRetryable("ConnectionTimeout"));
  EXPECT_TRUE(OssClient::IsRetryable("OperationTimeoutTime"));
}

TEST(OssClientTest, IsRetryableReturnsFalseForNonRetryableCodes) {
  EXPECT_FALSE(OssClient::IsRetryable(""));
  EXPECT_FALSE(OssClient::IsRetryable("NoSuchBucket"));
  EXPECT_FALSE(OssClient::IsRetryable("AccessDenied"));
  EXPECT_FALSE(OssClient::IsRetryable("InvalidArgument"));
  EXPECT_FALSE(OssClient::IsRetryable("FileNotFound"));
  EXPECT_FALSE(OssClient::IsRetryable("some_random_error"));
}

// ============================================================
// 构造函数/析构函数生命周期测试
// ============================================================

TEST(OssClientTest, ConstructorDestructorDoesNotCrash) {
  OssClient::Options opts;
  opts.access_key_id = "dummy";
  opts.access_key_secret = "dummy";
  opts.endpoint = "dummy";
  opts.bucket = "dummy";
  opts.max_retries = 0;

  for (int i = 0; i < 10; ++i) {
    OssClient client(opts);
    (void)client;
  }
}

TEST(OssClientTest, FromEnvThenConstruct) {
  auto opts = OssClient::Options::FromEnv();
  OssClient client(opts);
  (void)client;
}

// ============================================================
// 真实 OSS 上传集成测试（仅在环境变量配置时执行）
// ============================================================

TEST(OssClientTest, UploadSmallFileToOss) {
  const char* run_integration = std::getenv("MOULD_OSS_RUN_INTEGRATION_TEST");
  if (!run_integration || std::string(run_integration) != "true") {
    GTEST_SKIP() << "MOULD_OSS_RUN_INTEGRATION_TEST != true, skipping real OSS test";
  }

  auto opts = OssClient::Options::FromEnv();
  ASSERT_FALSE(opts.access_key_id.empty());
  ASSERT_FALSE(opts.access_key_secret.empty());
  ASSERT_FALSE(opts.endpoint.empty());
  ASSERT_FALSE(opts.bucket.empty());

  OssClient client(opts);

  std::unordered_map<std::string, std::string> metadata;
  metadata["request_id"] = "ut-oss-integration";
  metadata["object_key"] = "unittest/ut-oss-integration.jpg";

  std::string fake_jpeg(1024, 'A');

  std::promise<IBlobStorageUploader::UploadResult> result_promise;
  auto result_future = result_promise.get_future();

  client.UploadAsync(fake_jpeg, metadata,
      [&](const IBlobStorageUploader::UploadResult& result) {
        result_promise.set_value(result);
      });

  // 等待异步上传完成
  auto result = result_future.get();
  EXPECT_TRUE(result.success) << "code=" << result.error_code
                              << " msg=" << result.error_message;
  EXPECT_EQ(result.object_key, "unittest/ut-oss-integration.jpg");
  EXPECT_GE(result.duration_ms, 0);
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
