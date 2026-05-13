#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace mould {
namespace gate {
namespace http {

/// 与 muduo 无关的通用请求视图。
///
/// 设计原则：通用字段（method/path/body/headers）满足大多数场景，
/// 业务字段从 body 解析后通过 IngestHandler 内部流转。
struct RequestView {
    // ============================================================
    // 通用字段（新流水线优先使用）
    // ============================================================
    std::string request_id;                                    // 唯一标识（由网关生成）
    std::string method;                                        // HTTP 方法 (GET/POST/PUT/DELETE)
    std::string path;                                          // 请求路径（含 query string）
    std::string body;                                          // 原始请求体（通用）

    // HTTP 请求头部（路由层自动填充）
    std::unordered_map<std::string, std::string> headers;

    // ============================================================
    // 连接信息
    // ============================================================
    std::string client_ip;
    std::string content_type;
    int64_t content_length = 0;
    int64_t arrival_timestamp_ms = 0;                          // 请求到达时间戳（毫秒）
};

}  // namespace http
}  // namespace gate
}  // namespace mould
