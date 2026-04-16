#include "ms_logging.hpp"

#include <absl/strings/string_view.h>
#include <amqp.h>
#include <boost/version.hpp>
#include <gflags/gflags.h>
#include <grpcpp/grpcpp.h>
#include <muduo/base/Timestamp.h>
#include <mysql/mysql.h>
#include <onnxruntime_c_api.h>
#include <opencv2/core.hpp>
#include <alibabacloud/oss/OssClient.h>
#include <google/protobuf/stubs/common.h>

#include <string>

int main(int argc, char* argv[]) {
  (void)argc;
  gflags::SetUsageMessage("third_party_probe");
  mould::InitApplicationLogging(argv[0]);
  const auto protobuf_version =
      google::protobuf::internal::VersionString(GOOGLE_PROTOBUF_VERSION);

  auto channel = grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials());
  if (!channel) {
    LOG(ERROR) << "Failed to allocate gRPC channel";
    mould::ShutdownApplicationLogging();
    return 2;
  }

  absl::string_view tag = "third_party_probe";
  const char* mysql_version = mysql_get_client_info();
  const char* rabbitmq_version = amqp_version();
  const auto moduo_now = muduo::Timestamp::now().toString();
  cv::Mat cv_probe(1, 1, CV_8U);
  cv_probe.at<unsigned char>(0, 0) = 7;
  const OrtApiBase* ort_api_base = OrtGetApiBase();

  AlibabaCloud::OSS::ClientConfiguration cfg;
  const auto oss_cfg_size = sizeof(cfg);

  LOG(INFO) << "probe=" << tag << ", protobuf=" << protobuf_version
            << ", boost=" << BOOST_LIB_VERSION << ", mysql=" << mysql_version
            << ", rabbitmq=" << rabbitmq_version << ", moduo_now=" << moduo_now
            << ", cv_probe=" << static_cast<int>(cv_probe.at<unsigned char>(0, 0))
            << ", onnxruntime_api=" << (ort_api_base != nullptr)
            << ", oss_cfg_size=" << oss_cfg_size;
  mould::ShutdownApplicationLogging();
  return 0;
}
