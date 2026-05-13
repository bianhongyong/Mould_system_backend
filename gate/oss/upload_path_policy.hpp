#pragma once

#include <chrono>
#include <ctime>
#include <sstream>
#include <string>
#include <unordered_map>

namespace mould {
namespace gate {
namespace oss {

/// OSS 对象 key 生成策略接口。
class IUploadPathPolicy {
 public:
  virtual ~IUploadPathPolicy() = default;

  /// 根据元信息生成 OSS 对象 key。
  virtual std::string GeneratePath(
      const std::unordered_map<std::string, std::string>& metadata) = 0;
};

/// 默认路径策略。
/// 模板: images/{node_id}/{date}/{image_id}.jpg
/// date 格式: YYYY-MM-DD
class DefaultUploadPathPolicy : public IUploadPathPolicy {
 public:
  std::string GeneratePath(
      const std::unordered_map<std::string, std::string>& metadata) override {
    auto node_it = metadata.find("node_id");
    std::string node_id = (node_it != metadata.end()) ? node_it->second : "unknown_node";

    // 优先使用原始文件名（自带扩展名），其次 image_id/request_id + .jpg
    auto fn_it = metadata.find("image_filename");
    std::string filename;
    if (fn_it != metadata.end() && !fn_it->second.empty()) {
      filename = fn_it->second;
    } else {
      auto img_it = metadata.find("image_id");
      std::string image_id = (img_it != metadata.end() && !img_it->second.empty())
                                 ? img_it->second
                                 : [&]() -> std::string {
                                   auto rid = metadata.find("request_id");
                                   return (rid != metadata.end() && !rid->second.empty())
                                              ? rid->second
                                              : "unknown_image";
                                 }();
      filename = image_id + ".jpg";
    }

    std::string date_str = GetTodayDate();

    std::ostringstream oss;
    oss << "images/" << node_id << "/" << date_str << "/" << filename;
    return oss.str();
  }

 private:
  static std::string GetTodayDate() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[11] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf);
    return std::string(buf);
  }
};

}  // namespace oss
}  // namespace gate
}  // namespace mould
