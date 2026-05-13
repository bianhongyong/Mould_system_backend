#pragma once

#include <string>

namespace mould {
namespace gate {
namespace handler {

/// SHM 发布结果
struct ShmPublishResult {
    bool success = false;

    /// 错误类型
    enum class ErrorType { kNone, kBackpressure, kSlotFull, kOther };

    ErrorType error_type = ErrorType::kNone;
    std::string error_message;
    std::string image_id;  // SHM 分配的 ID（如果有）
};

/// SHM 帧发布器接口
///
/// 封装向共享内存发布帧数据的调用。
/// 具体实现在后续任务中接入真正的 ShmBusRuntime。
class IShmFramePublisher {
public:
    virtual ~IShmFramePublisher() = default;

    /// 发布帧数据到指定通道
    /// @param channel_name  目标 SHM 通道名称
    /// @param payload       序列化后的帧载荷
    virtual ShmPublishResult Publish(const std::string& channel_name,
                                      const std::string& payload) = 0;
};

/// Fake SHM 帧发布器（测试用）
///
/// 可配置成功/失败模式，记录发布次数和数据。
class FakeShmFramePublisher : public IShmFramePublisher {
public:
    ShmPublishResult Publish(const std::string& channel_name,
                              const std::string& payload) override {
        published_count_++;
        last_channel_ = channel_name;
        last_payload_ = payload;
        return result_to_return_;
    }

    int published_count_ = 0;
    std::string last_channel_;
    std::string last_payload_;
    ShmPublishResult result_to_return_;
};

}  // namespace handler
}  // namespace gate
}  // namespace mould
