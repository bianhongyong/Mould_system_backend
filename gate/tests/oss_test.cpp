#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>

#include "oss/iblob_storage_uploader.hpp"
#include "oss/local_file_uploader.hpp"
#include "oss/noop_uploader.hpp"
#include "oss/upload_path_policy.hpp"

namespace {

using mould::gate::oss::DefaultUploadPathPolicy;
using mould::gate::oss::IBlobStorageUploader;
using mould::gate::oss::IUploadPathPolicy;
using mould::gate::oss::LocalFileUploader;
using mould::gate::oss::NoOpUploader;

// ============================================================
// NoOpUploader
// ============================================================

TEST(OssTest, NoOpUploaderReturnsSuccess) {
  NoOpUploader uploader;

  std::unordered_map<std::string, std::string> metadata;
  metadata["request_id"] = "test-001";

  bool callback_called = false;
  uploader.UploadAsync(
      "dummy image data",
      metadata,
      [&](const IBlobStorageUploader::UploadResult& result) {
        callback_called = true;
        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.object_key, "noop/test-001");
        EXPECT_EQ(result.duration_ms, 0);
      });

  EXPECT_TRUE(callback_called);
}

TEST(OssTest, NoOpUploaderMissingRequestId) {
  NoOpUploader uploader;

  std::unordered_map<std::string, std::string> metadata;
  // 不设置 request_id

  bool callback_called = false;
  uploader.UploadAsync(
      "data",
      metadata,
      [&](const IBlobStorageUploader::UploadResult& result) {
        callback_called = true;
        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.object_key, "noop/unknown");
      });

  EXPECT_TRUE(callback_called);
}

TEST(OssTest, NoOpUploaderNullCallback) {
  // 不应崩溃
  NoOpUploader uploader;
  std::unordered_map<std::string, std::string> metadata;
  metadata["request_id"] = "null-cb";
  uploader.UploadAsync("data", metadata, nullptr);
}

// ============================================================
// LocalFileUploader
// ============================================================

class LocalFileUploaderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // 使用临时目录
    tmp_dir_ = "/tmp/mould_oss_test_" + std::to_string(::getpid());
    mkdir(tmp_dir_.c_str(), 0755);
  }

  void TearDown() override {
    // 清理临时目录下的文件
    std::string cmd = "rm -rf " + tmp_dir_;
    int rc = std::system(cmd.c_str());
    static_cast<void>(rc);
  }

  std::string tmp_dir_;
};

TEST_F(LocalFileUploaderTest, WritesFileSuccessfully) {
  LocalFileUploader uploader(tmp_dir_);

  std::unordered_map<std::string, std::string> metadata;
  metadata["request_id"] = "img-001";

  std::string expected_data = "fake-jpeg-binary-data-12345";

  bool callback_called = false;
  uploader.UploadAsync(
      expected_data,
      metadata,
      [&](const IBlobStorageUploader::UploadResult& result) {
        callback_called = true;
        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.object_key, "img-001.jpg");
      });

  EXPECT_TRUE(callback_called);

  // 验证文件存在且内容正确
  std::string file_path = tmp_dir_ + "/img-001.jpg";
  std::ifstream ifs(file_path, std::ios::binary);
  ASSERT_TRUE(ifs.is_open()) << "File not found: " << file_path;

  std::stringstream buf;
  buf << ifs.rdbuf();
  EXPECT_EQ(buf.str(), expected_data);
}

TEST_F(LocalFileUploaderTest, ReportsFailureOnBadDirectory) {
  LocalFileUploader uploader("/nonexistent/path/that/does/not/exist");

  std::unordered_map<std::string, std::string> metadata;
  metadata["request_id"] = "fail-img";

  bool callback_called = false;
  uploader.UploadAsync(
      "data",
      metadata,
      [&](const IBlobStorageUploader::UploadResult& result) {
        callback_called = true;
        EXPECT_FALSE(result.success);
        EXPECT_EQ(result.error_code, "IO_ERROR");
      });

  EXPECT_TRUE(callback_called);
}

TEST_F(LocalFileUploaderTest, MissingRequestId) {
  LocalFileUploader uploader(tmp_dir_);

  std::unordered_map<std::string, std::string> metadata;
  // 不设置 request_id

  bool callback_called = false;
  uploader.UploadAsync(
      "some data",
      metadata,
      [&](const IBlobStorageUploader::UploadResult& result) {
        callback_called = true;
        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.object_key, "unknown.jpg");
      });

  EXPECT_TRUE(callback_called);

  std::string file_path = tmp_dir_ + "/unknown.jpg";
  std::ifstream ifs(file_path);
  EXPECT_TRUE(ifs.good());
}

TEST_F(LocalFileUploaderTest, DurationMsIsPositive) {
  LocalFileUploader uploader(tmp_dir_);

  std::unordered_map<std::string, std::string> metadata;
  metadata["request_id"] = "duration-test";

  bool callback_called = false;
  uploader.UploadAsync(
      "data",
      metadata,
      [&](const IBlobStorageUploader::UploadResult& result) {
        callback_called = true;
        EXPECT_TRUE(result.success);
        // 写入操作应耗时 >= 0ms
        EXPECT_GE(result.duration_ms, 0);
      });

  EXPECT_TRUE(callback_called);
}

// ============================================================
// UploadPathPolicy
// ============================================================

TEST(OssTest, DefaultUploadPathPolicyGeneratesCorrectPath) {
  DefaultUploadPathPolicy policy;

  std::unordered_map<std::string, std::string> metadata;
  metadata["node_id"] = "camera-01";
  metadata["image_id"] = "frame-0042";

  std::string path = policy.GeneratePath(metadata);

  // 应匹配模板：images/{node_id}/{date}/{image_id}.jpg
  EXPECT_TRUE(path.find("images/") == 0) << "path=" << path;
  EXPECT_NE(path.find("camera-01"), std::string::npos) << "path=" << path;
  EXPECT_NE(path.find("frame-0042"), std::string::npos) << "path=" << path;
  // 应包含日期 YYYY-MM-DD
  EXPECT_NE(path.find("-"), std::string::npos) << "path=" << path;
  // 应以 .jpg 结尾
  EXPECT_TRUE(path.size() >= 4 && path.substr(path.size() - 4) == ".jpg")
      << "path=" << path;
}

TEST(OssTest, DefaultUploadPathPolicyMissingFields) {
  DefaultUploadPathPolicy policy;

  std::unordered_map<std::string, std::string> metadata;
  // 不设置 node_id 和 image_id

  std::string path = policy.GeneratePath(metadata);

  EXPECT_TRUE(path.find("images/unknown_node/") != std::string::npos)
      << "path=" << path;
  EXPECT_TRUE(path.find("/unknown_image.jpg") != std::string::npos)
      << "path=" << path;
}

TEST(OssTest, DefaultUploadPathPolicyDateIsToday) {
  DefaultUploadPathPolicy policy;

  std::unordered_map<std::string, std::string> metadata;
  metadata["node_id"] = "n1";
  metadata["image_id"] = "i1";

  std::string path = policy.GeneratePath(metadata);

  // 获取今天的日期字符串 YYYY-MM-DD
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm_buf{};
  localtime_r(&t, &tm_buf);
  char date_buf[11] = {};
  std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm_buf);
  std::string today(date_buf);

  EXPECT_NE(path.find(today), std::string::npos) << "path=" << path;
}

// ============================================================
// 接口多态性验证
// ============================================================

TEST(OssTest, PolymorphismViaInterface) {
  // 验证 NoOpUploader 可通过基类指针调用
  std::unique_ptr<IBlobStorageUploader> uploader =
      std::make_unique<NoOpUploader>();

  std::unordered_map<std::string, std::string> metadata;
  metadata["request_id"] = "poly-test";

  bool called = false;
  uploader->UploadAsync(
      "data",
      metadata,
      [&](const IBlobStorageUploader::UploadResult& result) {
        called = true;
        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.object_key, "noop/poly-test");
      });

  EXPECT_TRUE(called);
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
