#pragma once

#include <string>
#include <unordered_map>

#include <google/protobuf/util/message_differencer.h>

#include "gateway_frame_ingress.pb.h"

namespace mould {
namespace gate {
namespace handler {

/// 帧载荷构建器接口
///
/// 将校验通过的请求数据组装为 ImageFrameIngress proto 消息的序列化字节。
/// 公共字段直接映射到 proto 顶层字段，额外元数据写入 extra_metadata map。
class IFramePayloadBuilder {
public:
    virtual ~IFramePayloadBuilder() = default;

    /// 根据请求参数构建载荷
    /// @param request_id      请求唯一标识
    /// @param node_id         采集节点 ID
    /// @param capture_time    ISO 8601 格式采集时间
    /// @param image_data      图片二进制数据（by-value，调用方 std::move 传入以消除拷贝）
    /// @param image_format    图片格式 (jpeg/png/bmp)
    /// @param extra_metadata  额外元数据（场景差异化字段，写入 extra_metadata map）
    /// @return 序列化后的 ImageFrameIngress proto 字节
    virtual std::string BuildPayload(
        const std::string& request_id,
        const std::string& node_id,
        const std::string& capture_time,
        std::string image_data,
        const std::string& image_format,
        const std::unordered_map<std::string, std::string>& extra_metadata = {}) = 0;
};

/// ImageFramePayloadBuilder 实现
///
/// 使用 ImageFrameIngress proto 消息组装载荷：
///   - 公共字段 → request_id, node_id, capture_time, image_data, image_format
///   - 额外元数据 → extra_metadata map（场景差异化扩展的透传通道）
///
/// 场景扩展规范：
///   下游模块如需基于 extra_metadata 构建 typed proto，
///   应将对应 Any 消息添加到 ImageFrameIngress::scenario_extensions 中。
class ImageFramePayloadBuilder : public IFramePayloadBuilder {
public:
    std::string BuildPayload(
        const std::string& request_id,
        const std::string& node_id,
        const std::string& capture_time,
        std::string image_data,
        const std::string& image_format,
        const std::unordered_map<std::string, std::string>& extra_metadata = {}) override;
};

// ============================================================
// ImageFramePayloadBuilder 实现
// ============================================================

inline std::string ImageFramePayloadBuilder::BuildPayload(
    const std::string& request_id,
    const std::string& node_id,
    const std::string& capture_time,
    std::string image_data,
    const std::string& image_format,
    const std::unordered_map<std::string, std::string>& extra_metadata) {

    ::mould::gateway::ImageFrameIngress frame;

    frame.set_request_id(request_id);
    frame.set_node_id(node_id);
    frame.set_capture_time(capture_time);
    frame.set_image_format(image_format);
    // move 赋值避免 proto 内部大块数据拷贝
    frame.set_image_data(std::move(image_data));

    // 写入额外元数据（场景差异化字段透传）
    for (const auto& [key, value] : extra_metadata) {
        (*frame.mutable_extra_metadata())[key] = value;
    }

    return frame.SerializeAsString();
}

/// 辅助函数：从序列化字节解析 ImageFrameIngress
inline mould::gateway::ImageFrameIngress ParseFramePayload(const std::string& data) {
    ::mould::gateway::ImageFrameIngress frame;
    if (!frame.ParseFromString(data)) {
        throw std::runtime_error("ParseFramePayload: failed to parse ImageFrameIngress proto");
    }
    return frame;
}

}  // namespace handler
}  // namespace gate
}  // namespace mould
