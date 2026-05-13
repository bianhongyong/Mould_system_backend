#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include <alibabacloud/oss/OssClient.h>

#include "oss/oss_client.hpp"

namespace {

using mould::gate::oss::IBlobStorageUploader;
using mould::gate::oss::OssClient;

class OssStressTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    const char* flag = std::getenv("MOULD_OSS_RUN_INTEGRATION_TEST");
    if (!flag || std::string(flag) != "true") {
      GTEST_SKIP() << "MOULD_OSS_RUN_INTEGRATION_TEST != true, skipping";
      return;
    }

    opts_ = OssClient::Options::FromEnv();
    ASSERT_FALSE(opts_.access_key_id.empty());
    ASSERT_FALSE(opts_.access_key_secret.empty());
    ASSERT_FALSE(opts_.endpoint.empty());
    ASSERT_FALSE(opts_.bucket.empty());

    oss_sdk_client_ = std::make_unique<AlibabaCloud::OSS::OssClient>(
        opts_.endpoint, opts_.access_key_id, opts_.access_key_secret,
        AlibabaCloud::OSS::ClientConfiguration());

    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream ss;
    ss << "stress-test/" << std::put_time(&tm, "%Y%m%d-%H%M%S") << "-" << ::getpid() << "/";
    object_prefix_ = ss.str();

    std::cout << "\n[StressTest] prefix: " << object_prefix_ << std::endl;
  }

  static void TearDownTestSuite() {
    if (!oss_sdk_client_ || object_prefix_.empty()) return;

    std::cout << "[StressTest] cleaning up objects under " << object_prefix_ << " ..."
              << std::endl;

    AlibabaCloud::OSS::ListObjectsRequest request(opts_.bucket);
    request.setPrefix(object_prefix_);
    bool is_truncated = true;
    int deleted_count = 0;

    while (is_truncated) {
      auto outcome = oss_sdk_client_->ListObjects(request);
      if (!outcome.isSuccess()) {
        std::cerr << "[StressTest] ListObjects failed: " << outcome.error().Message()
                  << std::endl;
        break;
      }

      for (auto const& obj : outcome.result().ObjectSummarys()) {
        auto del_outcome = oss_sdk_client_->DeleteObject(opts_.bucket, obj.Key());
        if (del_outcome.isSuccess()) ++deleted_count;
      }

      is_truncated = outcome.result().IsTruncated();
      if (is_truncated) request.setMarker(outcome.result().NextMarker());
    }

    std::cout << "[StressTest] deleted " << deleted_count << " objects" << std::endl;
  }

  static int64_t UploadBatch(
      int pool_size,
      int file_count,
      const std::string& file_data,
      int round) {
    opts_.upload_threads = pool_size;
    OssClient client(opts_);

    std::atomic<int> completed{0};
    std::promise<void> done_promise;
    auto done_future = done_promise.get_future();

    auto start_ts = std::chrono::steady_clock::now();

    for (int i = 0; i < file_count; ++i) {
      std::unordered_map<std::string, std::string> metadata;
      metadata["request_id"] = "stress-r" + std::to_string(round) + "-" + std::to_string(i);
      metadata["object_key"] = object_prefix_ +
          "pool" + std::to_string(pool_size) + "/" +
          "r" + std::to_string(round) + "_file_" + std::to_string(i) + ".jpg";

      client.UploadAsync(file_data, metadata,
          [&](const IBlobStorageUploader::UploadResult& upload_result) {
            EXPECT_TRUE(upload_result.success)
                << "code=" << upload_result.error_code
                << " msg=" << upload_result.error_message;
            if (++completed == file_count) {
              done_promise.set_value();
            }
          });
    }

    done_future.wait();
    auto end_ts = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(end_ts - start_ts).count();
  }

  static int64_t MultipartUploadBatch(
      int pool_size,
      int file_count,
      const std::string& file_data,
      int round) {
    opts_.upload_threads = pool_size;
    OssClient client(opts_);

    std::atomic<int> completed{0};
    std::promise<void> done_promise;
    auto done_future = done_promise.get_future();

    auto start_ts = std::chrono::steady_clock::now();

    for (int i = 0; i < file_count; ++i) {
      std::unordered_map<std::string, std::string> metadata;
      metadata["request_id"] = "mp-stress-r" + std::to_string(round) + "-" + std::to_string(i);
      metadata["object_key"] = object_prefix_ +
          "mp-pool" + std::to_string(pool_size) + "/" +
          "r" + std::to_string(round) + "_file_" + std::to_string(i) + ".jpg";

      client.UploadMultipartAsync(file_data, metadata,
          [&](const IBlobStorageUploader::UploadResult& upload_result) {
            EXPECT_TRUE(upload_result.success)
                << "code=" << upload_result.error_code
                << " msg=" << upload_result.error_message;
            if (++completed == file_count) {
              done_promise.set_value();
            }
          });
    }

    done_future.wait();
    auto end_ts = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(end_ts - start_ts).count();
  }

  static OssClient::Options opts_;
  static std::unique_ptr<AlibabaCloud::OSS::OssClient> oss_sdk_client_;
  static std::string object_prefix_;
};

OssClient::Options OssStressTest::opts_;
std::unique_ptr<AlibabaCloud::OSS::OssClient> OssStressTest::oss_sdk_client_;
std::string OssStressTest::object_prefix_;

TEST_F(OssStressTest, StressUploadReport) {
  const std::vector<int> pool_sizes = {1, 2, 4, 8};
  const char* rounds_str = std::getenv("MOULD_OSS_STRESS_ROUNDS");
  const int rounds = rounds_str ? std::atoi(rounds_str) : 3;
  if (rounds < 1) return;

  std::string small_file(100 * 1024, 'S');
  int small_count = 100;

  std::string large_file(4 * 1024 * 1024, 'L');
  int large_count = 20;

  struct RunResult {
    std::vector<int64_t> small_ms;
    std::vector<int64_t> large_ms;
  };
  std::vector<RunResult> results(pool_sizes.size());

  for (size_t pi = 0; pi < pool_sizes.size(); ++pi) {
    int ps = pool_sizes[pi];
    std::cout << "\n[StressTest] pool_size=" << ps
              << " (" << rounds << " rounds)"
              << std::endl;

    for (int r = 0; r < rounds; ++r) {
      int64_t small_ms = UploadBatch(ps, small_count, small_file, r);
      results[pi].small_ms.push_back(small_ms);

      int64_t large_ms = UploadBatch(ps, large_count, large_file, r);
      results[pi].large_ms.push_back(large_ms);

      std::cout << "  round " << (r + 1) << ": small=" << small_ms << "ms"
                << "  large=" << large_ms << "ms" << std::endl;
    }

    auto calc = [](const std::vector<int64_t>& v) {
      if (v.empty()) return std::tuple<double, double, double>{0, 0, 0};
      double sum = std::accumulate(v.begin(), v.end(), 0.0);
      double avg = sum / v.size();
      double min = *std::min_element(v.begin(), v.end());
      double max = *std::max_element(v.begin(), v.end());
      return std::tuple{avg, min, max};
    };

    auto [s_avg, s_min, s_max] = calc(results[pi].small_ms);
    auto [l_avg, l_min, l_max] = calc(results[pi].large_ms);

    std::cout << "  => small: avg=" << s_avg << "ms"
              << "  min=" << s_min << "ms"
              << "  max=" << s_max << "ms" << std::endl;
    std::cout << "  => large: avg=" << l_avg << "ms"
              << "  min=" << l_min << "ms"
              << "  max=" << l_max << "ms" << std::endl;
  }

  // 汇总报告
  std::cout << "\n";
  std::cout << "======================================================================\n";
  std::cout << "       OSS Upload Stress Test Report  (" << rounds << " rounds avg)\n";
  std::cout << "======================================================================\n";
  std::cout << "Pool Sz |  100KB x100 files (avg / min / max)  |   4MB x20 files (avg / min / max)\n";
  std::cout << "--------+--------------------------------------+-----------------------------------\n";

  for (size_t pi = 0; pi < pool_sizes.size(); ++pi) {
    int ps = pool_sizes[pi];
    auto& r = results[pi];

    auto calc = [](const std::vector<int64_t>& v) {
      double sum = std::accumulate(v.begin(), v.end(), 0.0);
      double avg = sum / v.size();
      double min = *std::min_element(v.begin(), v.end());
      double max = *std::max_element(v.begin(), v.end());
      return std::tuple{avg, min, max};
    };

    auto [s_avg, s_min, s_max] = calc(r.small_ms);
    auto [l_avg, l_min, l_max] = calc(r.large_ms);

    std::cout << "  " << std::setw(2) << ps << "    | "
              << std::fixed << std::setprecision(1)
              << std::setw(6) << s_avg << " / "
              << std::setw(5) << s_min << " / "
              << std::setw(5) << s_max << " ms  | "
              << std::setw(6) << l_avg << " / "
              << std::setw(5) << l_min << " / "
              << std::setw(5) << l_max << " ms"
              << std::endl;
  }
  std::cout << "======================================================================\n";

  // ================================================================
  // 分片上传对比（仅 4MB 大文件）
  // ================================================================
  std::cout << "\n";
  std::cout << "======================================================================\n";
  std::cout << "   Multipart Upload vs Regular Upload  (4MB x20, " << rounds << " rounds avg)\n";
  std::cout << "======================================================================\n";
  std::cout << "Pool Sz |   Regular (avg / min / max)    |  Multipart (avg / min / max)\n";
  std::cout << "--------+-------------------------------+-------------------------------\n";

  for (size_t pi = 0; pi < pool_sizes.size(); ++pi) {
    int ps = pool_sizes[pi];
    std::vector<int64_t> mp_large_ms;

    std::cout << "\n[StressTest][MP] pool_size=" << ps
              << " (" << rounds << " rounds)" << std::endl;

    for (int r = 0; r < rounds; ++r) {
      int64_t ms = MultipartUploadBatch(ps, large_count, large_file, r);
      mp_large_ms.push_back(ms);
      std::cout << "  round " << (r + 1) << ": " << ms << "ms" << std::endl;
    }

    auto calc = [](const std::vector<int64_t>& v) {
      double sum = std::accumulate(v.begin(), v.end(), 0.0);
      double avg = sum / v.size();
      double min = *std::min_element(v.begin(), v.end());
      double max = *std::max_element(v.begin(), v.end());
      return std::tuple{avg, min, max};
    };

    auto [l_avg, l_min, l_max] = calc(results[pi].large_ms);
    auto [m_avg, m_min, m_max] = calc(mp_large_ms);

    std::cout << "  => regular:  avg=" << l_avg << "ms"
              << "  min=" << l_min << "ms"
              << "  max=" << l_max << "ms" << std::endl;
    std::cout << "  => multipart: avg=" << m_avg << "ms"
              << "  min=" << m_min << "ms"
              << "  max=" << m_max << "ms" << std::endl;

    std::cout << "  " << std::setw(2) << ps << "    | "
              << std::fixed << std::setprecision(1)
              << std::setw(6) << l_avg << " / "
              << std::setw(5) << l_min << " / "
              << std::setw(5) << l_max << " ms  | "
              << std::setw(6) << m_avg << " / "
              << std::setw(5) << m_min << " / "
              << std::setw(5) << m_max << " ms"
              << std::endl;
  }
  std::cout << "======================================================================\n";
}

}  // namespace

int main(int argc, char** argv) {
  setenv("GLOG_minloglevel", "1", 0);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
