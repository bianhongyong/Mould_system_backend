#pragma once

#include <any>
#include <string>

#include "gate/http/request_view.hpp"

namespace mould {
namespace gate {
namespace handler {

/// 请求处理结果（通用）
struct HandlerResult {
    bool success = false;
    int http_status_code = 500;
    std::string error_message;
    std::string request_id;
    std::string resource_id;   // 通用资源标识，图片场景即 image_id
};

/// 请求处理模板方法基类。
///
/// 固定骨架:
///   Handle()  ← 不可重写
///     ├── DoParse()    子类实现：解析 HTTP 请求 → 领域数据
///     ├── DoProcess()  子类实现：处理领域请求（SHM 发布/资源存储等）
///     └── PostProcess() 子类可选覆盖：异步后处理（不影响主响应）
///
/// 子类只需实现 DoParse + DoProcess，新增一种消息类型即新增一个子类。
class IHandler {
public:
    virtual ~IHandler() = default;

    /// 解析阶段结果
    struct ParseResult {
        bool ok = false;
        int status_code = 400;
        std::string error_msg;
        std::any ctx;   // 子类领域数据扩展槽，由 DoParse 写入、DoProcess/PostProcess 读取
    };

    /// 处理阶段结果
    struct ProcessResult {
        bool ok = false;
        int status_code = 500;
        std::string error_msg;
        std::string request_id;
        std::string resource_id;
        std::any ctx;   // 子类领域数据扩展槽，由 DoProcess 写入、PostProcess 读取
    };

    // ============================================================
    // Template Method — 固定骨架，子类不可重写
    // ============================================================
    HandlerResult Handle(http::RequestView& view) {
        HandlerResult result;

        // Stage 1: 解析
        auto parse_result = DoParse(view);
        if (!parse_result.ok) {
            result.success = false;
            result.http_status_code = parse_result.status_code;
            result.error_message = parse_result.error_msg;
            result.request_id = view.request_id;
            return result;
        }

        // Stage 2: 处理
        auto process_result = DoProcess(parse_result);
        if (!process_result.ok) {
            result.success = false;
            result.http_status_code = process_result.status_code;
            result.error_message = process_result.error_msg;
            result.request_id = view.request_id;
            return result;
        }

        // Stage 3: 异步后处理（不影响 HTTP 响应码）
        PostProcess(process_result);

        // 成功
        result.success = true;
        result.http_status_code = 200;
        result.request_id = process_result.request_id;
        result.resource_id = process_result.resource_id;
        return result;
    }

protected:
    // ============================================================
    // 子类实现的 hooks
    // ============================================================

    /// 解析 HTTP 请求为领域数据。校验也应在此阶段完成。
    virtual ParseResult DoParse(http::RequestView& view) = 0;

    /// 处理领域请求（发布 SHM、存储资源、下发命令等）。
    /// input 为 DoParse 返回的结果（子类可扩展此结构）。
    virtual ProcessResult DoProcess(const ParseResult& input) = 0;

    /// 后处理 hook。默认空，子类可按需覆盖。
    /// 在这里执行异步操作（如 OSS 上传），失败仅记日志，不影响主响应。
    virtual void PostProcess(const ProcessResult& /*result*/) {}
};

}  // namespace handler
}  // namespace gate
}  // namespace mould
