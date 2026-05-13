#pragma once

#include <functional>
#include <string>
#include <vector>

#include "gate/http/request_view.hpp"
#include "gate/handler/ihandler.hpp"

namespace mould {
namespace gate {

/// HTTP 路由分发器。
///
/// 根据 HTTP method + URL path 将请求分发到对应的 IHandler 或 HandlerFn。
/// 支持路径前缀匹配（/* 通配符后缀）。
///
/// 用法:
///   RouterDispatcher router;
///   router.RegisterRoute("POST", "/api/v1/frames/upload", my_handler);
///   router.RegisterRoute("GET", "/health", health_handler);
///   router.SetFallback(not_found_handler);
///   router.Dispatch(view);  // → HandlerResult
class RouterDispatcher {
public:
    using HandlerFn = std::function<handler::HandlerResult(
        http::RequestView&)>;

    /// 注册一个 IHandler 路由
    void RegisterRoute(const std::string& method,
                       const std::string& path_pattern,
                       handler::IHandler* handler) {
        routes_.push_back({
            method,
            path_pattern,
            [handler](http::RequestView& view) {
                return handler->Handle(view);
            }
        });
    }

    /// 注册一个 HandlerFn 路由（用于简单端点如 /health）
    void RegisterRoute(const std::string& method,
                       const std::string& path_pattern,
                       HandlerFn handler_fn) {
        routes_.push_back({method, path_pattern, std::move(handler_fn)});
    }

    /// 设置兜底 handler_fn（无匹配路由时调用，默认返回 404）
    void SetFallback(HandlerFn handler_fn) {
        fallback_ = std::move(handler_fn);
    }

    /// 分发请求：匹配路由 → 执行 handler_fn → 返回结果
    handler::HandlerResult Dispatch(http::RequestView& view) {
        for (const auto& route : routes_) {
            if (!MatchMethod(route.method, view.method)) {
                continue;
            }
            if (!MatchPath(route.path_pattern, view.path)) {
                continue;
            }
            return route.handler(view);
        }

        // 无匹配路由
        if (fallback_) {
            return fallback_(view);
        }
        return MakeNotFoundResult(view.request_id);
    }

private:
    struct RouteEntry {
        std::string method;
        std::string path_pattern;
        HandlerFn handler;
    };

    std::vector<RouteEntry> routes_;
    HandlerFn fallback_;

    static bool MatchMethod(const std::string& pattern,
                            const std::string& actual) {
        // 空或 * 匹配任意方法
        return pattern.empty() || pattern == "*" ||
               pattern == actual;
    }

    /// 路径匹配：支持精确匹配和 /* 前缀通配
    static bool MatchPath(const std::string& pattern,
                          const std::string& actual) {
        if (pattern == actual) {
            return true;
        }
        // /* 通配：匹配此前缀的所有路径
        if (pattern.size() >= 2 &&
            pattern.compare(pattern.size() - 2, 2, "/*") == 0) {
            std::string prefix = pattern.substr(0, pattern.size() - 2);
            if (prefix.empty()) {
                return true;  // /* 匹配所有
            }
            return actual.size() >= prefix.size() &&
                   actual.compare(0, prefix.size(), prefix) == 0 &&
                   (actual.size() == prefix.size() ||
                    actual[prefix.size()] == '/');
        }
        return false;
    }

    static handler::HandlerResult MakeNotFoundResult(
        const std::string& request_id) {
        handler::HandlerResult result;
        result.success = false;
        result.http_status_code = 404;
        result.error_message = "Not Found";
        result.request_id = request_id;
        return result;
    }
};

}  // namespace gate
}  // namespace mould
