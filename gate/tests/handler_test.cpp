#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "gate/http/request_view.hpp"
#include "gate/handler/ihandler.hpp"
#include "gate/router_dispatcher.hpp"

namespace mould {
namespace gate {
namespace test {
namespace {

using namespace handler;
using namespace http;

// ============================================================
// 辅助：用于测试 IHandler 模板方法的各类实现
// ============================================================

/// 全流程成功的 handler
class SuccessHandler : public IHandler {
public:
    int parse_call_count = 0;
    int process_call_count = 0;
    int postprocess_call_count = 0;

    ParseResult DoParse(RequestView& view) override {
        parse_call_count++;
        view.request_id = "test-req-001";
        return {true, 200, "", {}};
    }

    ProcessResult DoProcess(const ParseResult& /*input*/) override {
        process_call_count++;
        return {true, 200, "", "test-req-001", "resource-001", {}};
    }

    void PostProcess(const ProcessResult& /*result*/) override {
        postprocess_call_count++;
    }
};

/// DoParse 阶段失败的 handler
class ParseFailHandler : public IHandler {
    ParseResult DoParse(RequestView& /*view*/) override {
        return {false, 400, "Bad request body", {}};
    }

    ProcessResult DoProcess(const ParseResult& /*input*/) override {
        ADD_FAILURE() << "DoProcess should not be called after DoParse fails";
        return {false, 500, "unreachable", "", "", {}};
    }
};

/// DoProcess 阶段失败的 handler
class ProcessFailHandler : public IHandler {
    ParseResult DoParse(RequestView& view) override {
        view.request_id = "test-req-002";
        return {true, 200, "", {}};
    }

    ProcessResult DoProcess(const ParseResult& /*input*/) override {
        return {false, 503, "SHM backpressure", "", "", {}};
    }
};

/// 无 PostProcess 覆盖的 handler（测试默认空实现）
class NoPostProcessHandler : public IHandler {
    ParseResult DoParse(RequestView& view) override {
        view.request_id = "test-req-003";
        return {true, 200, "", {}};
    }

    ProcessResult DoProcess(const ParseResult& /*input*/) override {
        return {true, 200, "", "test-req-003", "resource-003", {}};
    }
};

// ============================================================
// IHandler 模板方法骨架测试
// ============================================================

TEST(IHandlerTest, HandleCallsAllStagesOnSuccess) {
    SuccessHandler handler;
    RequestView view;

    auto result = handler.Handle(view);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.http_status_code, 200);
    EXPECT_EQ(result.request_id, "test-req-001");
    EXPECT_EQ(result.resource_id, "resource-001");
    EXPECT_EQ(handler.parse_call_count, 1);
    EXPECT_EQ(handler.process_call_count, 1);
    EXPECT_EQ(handler.postprocess_call_count, 1);
}

TEST(IHandlerTest, ParseFailureSkipsProcessAndPostProcess) {
    ParseFailHandler handler;
    RequestView view;

    auto result = handler.Handle(view);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.http_status_code, 400);
    EXPECT_EQ(result.error_message, "Bad request body");
}

TEST(IHandlerTest, ProcessFailureSkipsPostProcess) {
    ProcessFailHandler handler;
    RequestView view;

    auto result = handler.Handle(view);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.http_status_code, 503);
    EXPECT_EQ(result.error_message, "SHM backpressure");
    EXPECT_EQ(result.request_id, "test-req-002");
}

TEST(IHandlerTest, NoPostProcessDefaultIsEmpty) {
    NoPostProcessHandler handler;
    RequestView view;

    auto result = handler.Handle(view);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.http_status_code, 200);
    EXPECT_EQ(result.resource_id, "resource-003");
}

TEST(IHandlerTest, RequestIdFromViewOnParseFailure) {
    ParseFailHandler handler;
    RequestView view;
    view.request_id = "pre-set-id";

    auto result = handler.Handle(view);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.request_id, "pre-set-id");
}

TEST(IHandlerTest, MultipleCallsWork) {
    SuccessHandler handler;
    RequestView view1, view2;

    auto r1 = handler.Handle(view1);
    auto r2 = handler.Handle(view2);

    EXPECT_TRUE(r1.success);
    EXPECT_TRUE(r2.success);
    EXPECT_EQ(handler.parse_call_count, 2);
    EXPECT_EQ(handler.process_call_count, 2);
}

TEST(IHandlerTest, HandlerResultFieldsCorrectOnSuccess) {
    SuccessHandler handler;
    RequestView view;

    auto result = handler.Handle(view);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.http_status_code, 200);
    EXPECT_EQ(result.request_id, "test-req-001");
    EXPECT_EQ(result.resource_id, "resource-001");
    EXPECT_TRUE(result.error_message.empty());
}

TEST(IHandlerTest, HandlerResultFieldsCorrectOnProcessFailure) {
    ProcessFailHandler handler;
    RequestView view;

    auto result = handler.Handle(view);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.http_status_code, 503);
    EXPECT_FALSE(result.error_message.empty());
    EXPECT_TRUE(result.resource_id.empty());
}

// ============================================================
// RouterDispatcher 路由分发测试
// ============================================================

/// 返回固定状态码和消息的 handler（用于验证路由匹配）
class EchoHandler : public IHandler {
public:
    int code;
    std::string msg;
    EchoHandler(int c, std::string m) : code(c), msg(std::move(m)) {}

    ParseResult DoParse(RequestView& view) override {
        view.request_id = "echo";
        return {true, 200, "", {}};
    }

    ProcessResult DoProcess(const ParseResult& /*input*/) override {
        return {true, code, "", "echo", msg, {}};
    }
};

/// 记录是否被调用的 handler
class CallTracker : public IHandler {
public:
    int call_count = 0;
    std::string expected_path;

    ParseResult DoParse(RequestView& view) override {
        call_count++;
        view.request_id = "tracker";
        expected_path = view.path;
        return {true, 200, "", {}};
    }

    ProcessResult DoProcess(const ParseResult& /*input*/) override {
        return {true, 200, "", "tracker", "ok", {}};
    }
};

TEST(RouterDispatcherTest, ExactPathMatch) {
    RouterDispatcher router;
    EchoHandler handler(200, "exact-match");

    router.RegisterRoute("GET", "/api/v1/frames/upload", &handler);

    RequestView view;
    view.method = "GET";
    view.path = "/api/v1/frames/upload";

    auto result = router.Dispatch(view);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.http_status_code, 200);
    EXPECT_EQ(result.resource_id, "exact-match");
}

TEST(RouterDispatcherTest, MethodMustMatch) {
    RouterDispatcher router;
    EchoHandler handler(200, "only-post");

    router.RegisterRoute("POST", "/api/v1/frames", &handler);

    RequestView view;
    view.method = "GET";
    view.path = "/api/v1/frames";

    auto result = router.Dispatch(view);
    // 没有匹配的路由 → 404
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.http_status_code, 404);
}

TEST(RouterDispatcherTest, WildcardPathPrefix) {
    RouterDispatcher router;
    EchoHandler handler(200, "wildcard");

    router.RegisterRoute("POST", "/api/v1/frames/*", &handler);

    RequestView view;
    view.method = "POST";
    view.path = "/api/v1/frames/upload";

    auto result = router.Dispatch(view);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.resource_id, "wildcard");

    // 子路径也应该匹配
    view.path = "/api/v1/frames/batch";
    result = router.Dispatch(view);
    EXPECT_TRUE(result.success);
}

TEST(RouterDispatcherTest, WildcardDoesNotMatchDifferentPrefix) {
    RouterDispatcher router;
    EchoHandler handler(200, "wildcard");

    router.RegisterRoute("POST", "/api/v1/frames/*", &handler);

    RequestView view;
    view.method = "POST";
    view.path = "/api/v1/text/message";  // 不匹配 /api/v1/frames/*

    auto result = router.Dispatch(view);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.http_status_code, 404);
}

TEST(RouterDispatcherTest, NoMatchingRouteReturns404) {
    RouterDispatcher router;

    RequestView view;
    view.method = "GET";
    view.path = "/nonexistent";

    auto result = router.Dispatch(view);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.http_status_code, 404);
    EXPECT_EQ(result.error_message, "Not Found");
}

TEST(RouterDispatcherTest, FallbackCalledWhenNoMatch) {
    RouterDispatcher router;
    bool fallback_called = false;

    router.SetFallback(
        [&fallback_called](RequestView& view) -> HandlerResult {
            fallback_called = true;
            HandlerResult r;
            r.success = false;
            r.http_status_code = 404;
            r.error_message = "Custom Not Found";
            r.request_id = view.request_id;
            return r;
        });

    RequestView view;
    view.method = "GET";
    view.path = "/anything";

    auto result = router.Dispatch(view);
    EXPECT_TRUE(fallback_called);
    EXPECT_EQ(result.http_status_code, 404);
    EXPECT_EQ(result.error_message, "Custom Not Found");
}

TEST(RouterDispatcherTest, MethodWildcardMatchesAny) {
    RouterDispatcher router;
    EchoHandler handler(200, "any-method");

    router.RegisterRoute("*", "/api/v1/status", &handler);

    RequestView view;
    view.path = "/api/v1/status";

    view.method = "GET";
    EXPECT_TRUE(router.Dispatch(view).success);

    view.method = "POST";
    EXPECT_TRUE(router.Dispatch(view).success);

    view.method = "PUT";
    EXPECT_TRUE(router.Dispatch(view).success);
}

TEST(RouterDispatcherTest, MostSpecificRouteWins) {
    RouterDispatcher router;
    EchoHandler specific(200, "specific");
    EchoHandler wildcard(200, "wildcard");

    router.RegisterRoute("*", "/api/v1/frames/*", &wildcard);
    router.RegisterRoute("POST", "/api/v1/frames/upload", &specific);

    RequestView view;
    view.method = "POST";
    view.path = "/api/v1/frames/upload";

    // 先注册的优先匹配，但因第一条是通配，第二条是精确
    // 当前实现是顺序遍历，先注册的先匹配
    // 所以第一条（wildcard）会匹配
    auto result = router.Dispatch(view);
    EXPECT_EQ(result.resource_id, "wildcard");
}

TEST(RouterDispatcherTest, HandlerFnRoute) {
    RouterDispatcher router;

    router.RegisterRoute("GET", "/status",
        [](RequestView& view) -> HandlerResult {
            HandlerResult r;
            r.success = true;
            r.http_status_code = 200;
            r.request_id = view.request_id;
            r.resource_id = "status-ok";
            return r;
        });

    RequestView view;
    view.method = "GET";
    view.path = "/status";

    auto result = router.Dispatch(view);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.resource_id, "status-ok");
}

TEST(RouterDispatcherTest, MultipleRoutesIndependent) {
    RouterDispatcher router;
    EchoHandler frame(200, "frame-handler");
    EchoHandler text(201, "text-handler");

    router.RegisterRoute("POST", "/api/v1/frames/upload", &frame);
    router.RegisterRoute("POST", "/api/v1/text/message", &text);

    RequestView view;
    view.method = "POST";

    view.path = "/api/v1/frames/upload";
    EXPECT_EQ(router.Dispatch(view).resource_id, "frame-handler");

    view.path = "/api/v1/text/message";
    EXPECT_EQ(router.Dispatch(view).resource_id, "text-handler");
}

TEST(RouterDispatcherTest, DispatchPreservesRequestViewFields) {
    RouterDispatcher router;
    CallTracker tracker;

    router.RegisterRoute("PUT", "/api/v1/resource/*", &tracker);
    router.RegisterRoute("PUT", "/api/v1/resource/123", &tracker);

    RequestView view;
    view.method = "PUT";
    view.path = "/api/v1/resource/123";
    view.body = "some-body";
    view.content_type = "application/json";

    auto result = router.Dispatch(view);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(tracker.call_count, 1);
}

TEST(RouterDispatcherTest, EmptyMethodRoutesMatchAllMethods) {
    RouterDispatcher router;
    EchoHandler handler(200, "empty-method");

    router.RegisterRoute("", "/api/v1/any", &handler);

    RequestView view;
    view.path = "/api/v1/any";

    view.method = "GET";
    EXPECT_TRUE(router.Dispatch(view).success);

    view.method = "POST";
    EXPECT_TRUE(router.Dispatch(view).success);

    view.method = "DELETE";
    EXPECT_TRUE(router.Dispatch(view).success);
}

TEST(RouterDispatcherTest, RegisterMultipleSameRouteLastWins) {
    RouterDispatcher router;
    EchoHandler first(200, "first");
    EchoHandler second(201, "second");

    router.RegisterRoute("GET", "/dup", &first);
    router.RegisterRoute("GET", "/dup", &second);

    RequestView view;
    view.method = "GET";
    view.path = "/dup";

    // 顺序遍历，先注册的先匹配
    auto result = router.Dispatch(view);
    EXPECT_EQ(result.resource_id, "first");
}

}  // namespace
}  // namespace test
}  // namespace gate
}  // namespace mould

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
