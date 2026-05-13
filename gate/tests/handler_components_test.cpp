#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gate/handler/body_parser.hpp"
#include "gate/handler/frame_payload_builder.hpp"
#include "gate/handler/ingest_handler.hpp"
#include "gate/handler/metrics_recorder.hpp"
#include "gate/handler/rate_limiter.hpp"
#include "gate/handler/request_validator.hpp"
#include "gate/handler/shm_frame_publisher.hpp"
#include "gate/handler/uuid_generator.hpp"
#include "gate/oss/noop_uploader.hpp"

namespace mould {
namespace gate {
namespace handler {
namespace {

// ============================================================
// MultipartFormDataParser 测试
// ============================================================

/// 构建简单的 multipart/form-data 请求体
static std::string BuildMultipartBody(
    const std::string& boundary,
    const std::vector<std::pair<std::string, std::string>>& text_fields,
    const std::string& image_field_name,
    const std::string& image_data,
    const std::string& image_content_type) {
    std::ostringstream body;

    for (const auto& [name, value] : text_fields) {
        body << "--" << boundary << "\r\n"
             << "Content-Disposition: form-data; name=\"" << name << "\"\r\n\r\n"
             << value << "\r\n";
    }

    if (!image_field_name.empty()) {
        body << "--" << boundary << "\r\n"
             << "Content-Disposition: form-data; name=\"" << image_field_name
             << "\"; filename=\"test." << (image_content_type.find("jpeg") != std::string::npos ? "jpg" :
                 image_content_type.find("png") != std::string::npos ? "png" : "bmp")
             << "\"\r\n"
             << "Content-Type: " << image_content_type << "\r\n\r\n"
             << image_data << "\r\n";
    }

    body << "--" << boundary << "--\r\n";
    return body.str();
}

TEST(HandlerBodyParserTest, ParseValidMultipartJpeg) {
    MultipartFormDataParser parser;
    std::string boundary = "----TestBoundary123";
    std::string content_type =
        "multipart/form-data; boundary=" + boundary;

    std::string image_data;
    image_data += static_cast<char>(0xFF);
    image_data += static_cast<char>(0xD8);
    image_data += static_cast<char>(0xFF);
    image_data += static_cast<char>(0xE0);
    image_data += "jpeg_binary_data_here";

    std::string body = BuildMultipartBody(
        boundary,
        {{"node_id", "camera-01"}, {"capture_time", "2026-05-08T10:30:00Z"}},
        "image", image_data, "image/jpeg");

    ParseResult result = parser.Parse(body, content_type);

    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.image_format, "jpeg");
    EXPECT_EQ(result.image_data, image_data);
    ASSERT_EQ(result.fields.size(), 2);
    EXPECT_EQ(result.fields["node_id"], "camera-01");
    EXPECT_EQ(result.fields["capture_time"], "2026-05-08T10:30:00Z");
}

TEST(HandlerBodyParserTest, ParseValidMultipartPng) {
    MultipartFormDataParser parser;
    std::string boundary = "----TestBoundary456";
    std::string content_type =
        "multipart/form-data; boundary=" + boundary;

    std::string image_data;
    image_data += static_cast<char>(0x89);
    image_data += 'P';
    image_data += 'N';
    image_data += 'G';
    image_data += "\r\n\x1a\n";
    image_data += "png_binary_data";

    std::string body = BuildMultipartBody(
        boundary,
        {{"node_id", "cam-02"}, {"capture_time", "2026-05-08T12:00:00Z"}},
        "image", image_data, "image/png");

    ParseResult result = parser.Parse(body, content_type);

    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.image_format, "png");
    EXPECT_EQ(result.image_data, image_data);
    EXPECT_EQ(result.fields["node_id"], "cam-02");
}

TEST(HandlerBodyParserTest, ParseValidMultipartBmp) {
    MultipartFormDataParser parser;
    std::string boundary = "----TestBoundaryBmp";
    std::string content_type =
        "multipart/form-data; boundary=" + boundary;

    std::string image_data;
    image_data += static_cast<char>(0x42);
    image_data += static_cast<char>(0x4D);
    image_data += "bmp_binary_data";

    std::string body = BuildMultipartBody(
        boundary,
        {{"node_id", "cam-03"}, {"capture_time", "2026-05-08T14:30:00Z"}},
        "image", image_data, "image/bmp");

    ParseResult result = parser.Parse(body, content_type);

    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.image_format, "bmp");
    EXPECT_EQ(result.image_data, image_data);
}

TEST(HandlerBodyParserTest, ParseMissingBoundary) {
    MultipartFormDataParser parser;
    std::string body = "some body content";
    std::string content_type = "multipart/form-data";

    ParseResult result = parser.Parse(body, content_type);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error_message.empty());
}

TEST(HandlerBodyParserTest, ParseEmptyBody) {
    MultipartFormDataParser parser;
    ParseResult result = parser.Parse("", "multipart/form-data; boundary=xxx");
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error_message.empty());
}

TEST(HandlerBodyParserTest, ParseMissingImageField) {
    MultipartFormDataParser parser;
    std::string boundary = "----BoundaryNoImage";
    std::string content_type =
        "multipart/form-data; boundary=" + boundary;

    // 只有文本字段，没有 image 字段
    std::string body;
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"node_id\"\r\n\r\n";
    body += "camera-01\r\n";
    body += "--" + boundary + "--\r\n";

    ParseResult result = parser.Parse(body, content_type);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_message, "Missing 'image' field in multipart data");
}

TEST(HandlerBodyParserTest, ParseImageWithoutContentType) {
    // 没有 Content-Type 头的图片字段，应通过 magic bytes 自动检测格式
    MultipartFormDataParser parser;
    std::string boundary = "----BoundaryMagicDetect";
    std::string content_type =
        "multipart/form-data; boundary=" + boundary;

    std::string image_data;
    image_data += static_cast<char>(0xFF);
    image_data += static_cast<char>(0xD8);
    image_data += static_cast<char>(0xFF);
    image_data += "no_content_type_header";

    std::string body;
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"node_id\"\r\n\r\n";
    body += "cam-01\r\n";
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"image\"; filename=\"test.jpg\"\r\n\r\n";
    body += image_data;
    body += "\r\n";
    body += "--" + boundary + "--\r\n";

    ParseResult result = parser.Parse(body, content_type);
    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.image_format, "jpeg");
}

TEST(HandlerBodyParserTest, ExtractBoundaryWithQuotes) {
    MultipartFormDataParser parser;
    // 测试带引号的 boundary
    std::string content_type =
        "multipart/form-data; boundary=\"----BoundaryInQuotes\"";

    // 使用反射测试 ExtractBoundary
    // 通过解析完整请求进行间接测试
    std::string body;
    body += "------BoundaryInQuotes\r\n";
    body += "Content-Disposition: form-data; name=\"image\"; filename=\"t.jpg\"\r\n";
    body += "Content-Type: image/jpeg\r\n\r\n";
    body += "\xFF\xD8\xFF";
    body += "\r\n";
    body += "------BoundaryInQuotes--\r\n";

    ParseResult result = parser.Parse(body, content_type);
    EXPECT_TRUE(result.success) << result.error_message;
}

TEST(HandlerBodyParserTest, ParseMultipleTextFields) {
    MultipartFormDataParser parser;
    std::string boundary = "----MultiFieldBoundary";
    std::string content_type =
        "multipart/form-data; boundary=" + boundary;

    std::string image_data;
    image_data += static_cast<char>(0xFF);
    image_data += static_cast<char>(0xD8);
    image_data += static_cast<char>(0xFF);

    std::string body = BuildMultipartBody(
        boundary,
        {{"node_id", "cam-01"},
         {"capture_time", "2026-05-08T10:30:00Z"},
         {"batch_id", "batch-20260508"},
         {"image_id", "frame-0042"}},
        "image", image_data, "image/jpeg");

    ParseResult result = parser.Parse(body, content_type);
    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.fields.size(), 4);
    EXPECT_EQ(result.fields["node_id"], "cam-01");
    EXPECT_EQ(result.fields["capture_time"], "2026-05-08T10:30:00Z");
    EXPECT_EQ(result.fields["batch_id"], "batch-20260508");
    EXPECT_EQ(result.fields["image_id"], "frame-0042");
}

TEST(HandlerBodyParserTest, ParseImageFormatDetectionWithoutContentType) {
    // 多种图片格式的 magic bytes 检测
    MultipartFormDataParser parser;
    std::string boundary = "----MagicByteTest";
    std::string content_type =
        "multipart/form-data; boundary=" + boundary;

    // PNG without Content-Type header (auto-detect)
    std::string png_data;
    png_data += static_cast<char>(0x89);
    png_data += 'P';
    png_data += 'N';
    png_data += 'G';
    png_data += "\r\n\x1a\n";

    std::string body;
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"image\"; filename=\"test.png\"\r\n\r\n";
    body += png_data;
    body += "\r\n";
    body += "--" + boundary + "--\r\n";

    ParseResult result = parser.Parse(body, content_type);
    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.image_format, "png");
}

// ============================================================
// RequestValidator 测试
// ============================================================

TEST(HandlerValidatorTest, ValidateAllFieldsValid) {
    RequestValidator validator;
    std::string image_data;
    image_data += static_cast<char>(0xFF);
    image_data += static_cast<char>(0xD8);
    image_data += static_cast<char>(0xFF);
    image_data += static_cast<char>(0xE0);

    ValidationResult result =
        validator.Validate("camera-01", "2026-05-08T10:30:00Z",
                           image_data, "jpeg");
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.http_status_code, 200);
    EXPECT_TRUE(result.missing_fields.empty());
    EXPECT_TRUE(result.invalid_fields.empty());
}

TEST(HandlerValidatorTest, ValidatePngFormat) {
    RequestValidator validator;
    std::string image_data;
    image_data += static_cast<char>(0x89);
    image_data += 'P';
    image_data += 'N';
    image_data += 'G';

    ValidationResult result =
        validator.Validate("cam-01", "2026-05-08T10:30:00Z",
                           image_data, "png");
    EXPECT_TRUE(result.valid);
}

TEST(HandlerValidatorTest, ValidateBmpFormat) {
    RequestValidator validator;
    std::string image_data;
    image_data += static_cast<char>(0x42);
    image_data += static_cast<char>(0x4D);

    ValidationResult result =
        validator.Validate("cam-01", "2026-05-08T10:30:00Z",
                           image_data, "bmp");
    EXPECT_TRUE(result.valid);
}

TEST(HandlerValidatorTest, ValidateMissingNodeId) {
    RequestValidator validator;
    ValidationResult result =
        validator.Validate("", "2026-05-08T10:30:00Z",
                           "fake_image_data", "jpeg");
    EXPECT_FALSE(result.valid);
    ASSERT_EQ(result.missing_fields.size(), 1);
    EXPECT_EQ(result.missing_fields[0], "node_id");
    EXPECT_EQ(result.http_status_code, 422);
}

TEST(HandlerValidatorTest, ValidateMissingCaptureTime) {
    RequestValidator validator;
    ValidationResult result =
        validator.Validate("camera-01", "",
                           "fake_image_data", "jpeg");
    EXPECT_FALSE(result.valid);
    ASSERT_EQ(result.missing_fields.size(), 1);
    EXPECT_EQ(result.missing_fields[0], "capture_time");
}

TEST(HandlerValidatorTest, ValidateMissingImageData) {
    RequestValidator validator;
    ValidationResult result =
        validator.Validate("camera-01", "2026-05-08T10:30:00Z",
                           "", "jpeg");
    EXPECT_FALSE(result.valid);
    ASSERT_EQ(result.missing_fields.size(), 1);
    EXPECT_EQ(result.missing_fields[0], "image_data");
}

TEST(HandlerValidatorTest, ValidateAllFieldsMissing) {
    RequestValidator validator;
    ValidationResult result = validator.Validate("", "", "", "");
    EXPECT_FALSE(result.valid);
    ASSERT_EQ(result.missing_fields.size(), 3);
    EXPECT_EQ(result.http_status_code, 422);
}

TEST(HandlerValidatorTest, ValidateInvalidCaptureTimeFormat_WrongSeparator) {
    RequestValidator validator;
    // 使用 '/' 而非 '-'
    ValidationResult result =
        validator.Validate("cam-01", "2026/05/08T10:30:00Z",
                           "data", "jpeg");
    EXPECT_FALSE(result.valid);
    ASSERT_FALSE(result.invalid_fields.empty());
    EXPECT_EQ(result.invalid_fields[0], "capture_time");
}

TEST(HandlerValidatorTest, ValidateInvalidCaptureTimeFormat_NoTimezone) {
    RequestValidator validator;
    ValidationResult result =
        validator.Validate("cam-01", "2026-05-08T10:30:00",
                           "data", "jpeg");
    EXPECT_FALSE(result.valid);
    ASSERT_FALSE(result.invalid_fields.empty());
}

TEST(HandlerValidatorTest, ValidateInvalidCaptureTimeFormat_TooShort) {
    RequestValidator validator;
    ValidationResult result =
        validator.Validate("cam-01", "2026-05-08",
                           "data", "jpeg");
    EXPECT_FALSE(result.valid);
}

TEST(HandlerValidatorTest, ValidateValidCaptureTimeWithFractionalSeconds) {
    RequestValidator validator;
    // 带小数秒和 Z 时区，需提供有效 JPEG magic bytes
    std::string jpeg_data = {'\xFF', '\xD8', '\xFF', '\x00'};
    ValidationResult result =
        validator.Validate("cam-01", "2026-05-08T10:30:00.123Z",
                           jpeg_data, "jpeg");
    EXPECT_TRUE(result.valid);
}

TEST(HandlerValidatorTest, ValidateValidCaptureTimeWithOffset) {
    RequestValidator validator;
    // 带时区偏移，需提供有效 JPEG magic bytes
    std::string jpeg_data = {'\xFF', '\xD8', '\xFF', '\x00'};
    ValidationResult result =
        validator.Validate("cam-01", "2026-05-08T10:30:00+08:00",
                           jpeg_data, "jpeg");
    EXPECT_TRUE(result.valid);
}

TEST(HandlerValidatorTest, ValidateValidCaptureTimeWithNegativeOffset) {
    RequestValidator validator;
    std::string jpeg_data = {'\xFF', '\xD8', '\xFF', '\x00'};
    ValidationResult result =
        validator.Validate("cam-01", "2026-05-08T10:30:00-05:00",
                           jpeg_data, "jpeg");
    EXPECT_TRUE(result.valid);
}

TEST(HandlerValidatorTest, ValidateFieldLengthExceeded) {
    RequestValidator validator(10);  // 最大字段长度为 10
    ValidationResult result =
        validator.Validate("very_long_node_id_12345",
                           "2026-05-08T10:30:00Z",
                           "data", "jpeg");
    EXPECT_FALSE(result.valid);
    ASSERT_FALSE(result.invalid_fields.empty());
    EXPECT_EQ(result.invalid_fields[0], "node_id");
}

TEST(HandlerValidatorTest, ValidateInvalidImageMagicBytes) {
    RequestValidator validator;
    std::string bad_image = "this is not an image file";
    ValidationResult result =
        validator.Validate("cam-01", "2026-05-08T10:30:00Z",
                           bad_image, "jpeg");
    EXPECT_FALSE(result.valid);
    EXPECT_FALSE(result.image_format_error.empty());
    EXPECT_EQ(result.http_status_code, 415);
}

TEST(HandlerValidatorTest, ValidateImageFormatMismatch) {
    RequestValidator validator;
    std::string image_data;
    image_data += static_cast<char>(0xFF);
    image_data += static_cast<char>(0xD8);
    image_data += static_cast<char>(0xFF);

    // 声明为 png 但实际是 jpeg
    ValidationResult result =
        validator.Validate("cam-01", "2026-05-08T10:30:00Z",
                           image_data, "png");
    EXPECT_FALSE(result.valid);
    EXPECT_FALSE(result.image_format_error.empty());
    EXPECT_TRUE(result.image_format_error.find("mismatch") != std::string::npos);
}

TEST(HandlerValidatorTest, ValidateEmptyImageFormatSkipsMagicCheck) {
    // image_format 为空字符串时，跳过 magic bytes 校验
    RequestValidator validator;
    std::string image_data = "some_random_data_without_magic_bytes";

    ValidationResult result =
        validator.Validate("cam-01", "2026-05-08T10:30:00Z",
                           image_data, "");
    // 应该通过，因为 format 为空时不做校验
    EXPECT_TRUE(result.valid);
}

// ============================================================
// FramePayload / HttpFrameIngress proto 测试
// ============================================================

TEST(HandlerFramePayloadTest, BuildPayloadProducesParsableProto) {
    ImageFramePayloadBuilder builder;
    std::string serialized = builder.BuildPayload(
        "gw-1715231400000-a1b2c3d4", "camera-01",
        "2026-05-08T10:30:00Z",
        "\xFF\xD8\xFF", "jpeg");

    EXPECT_FALSE(serialized.empty());

    ::mould::gateway::ImageFrameIngress frame;
    ASSERT_TRUE(frame.ParseFromString(serialized));
    EXPECT_EQ(frame.request_id(), "gw-1715231400000-a1b2c3d4");
    EXPECT_EQ(frame.node_id(), "camera-01");
    EXPECT_EQ(frame.capture_time(), "2026-05-08T10:30:00Z");
    EXPECT_EQ(frame.image_data(), "\xFF\xD8\xFF");
    EXPECT_EQ(frame.image_format(), "jpeg");
    EXPECT_TRUE(frame.extra_metadata().empty());
}

TEST(HandlerFramePayloadTest, BuildPayloadWithExtraMetadata) {
    ImageFramePayloadBuilder builder;
    std::unordered_map<std::string, std::string> extra = {
        {"batch_id", "batch-20260508"},
        {"product_type", "type-A"},
    };
    std::string serialized = builder.BuildPayload(
        "gw-12345-0001", "camera-01",
        "2026-05-08T10:30:00Z",
        "\xFF\xD8\xFF", "jpeg", extra);

    ::mould::gateway::ImageFrameIngress frame;
    ASSERT_TRUE(frame.ParseFromString(serialized));
    EXPECT_EQ(frame.extra_metadata().at("batch_id"), "batch-20260508");
    EXPECT_EQ(frame.extra_metadata().at("product_type"), "type-A");
    EXPECT_EQ(frame.extra_metadata_size(), 2);
}

TEST(HandlerFramePayloadTest, BinaryDataPreservation) {
    ImageFramePayloadBuilder builder;
    std::string binary_data;
    binary_data += static_cast<char>(0xFF);
    binary_data += static_cast<char>(0xD8);
    binary_data += static_cast<char>(0xFF);
    binary_data += static_cast<char>(0x00);  // 空字节
    binary_data += static_cast<char>(0xE0);
    binary_data += "\x01\x02\x03\xFF\xFE\xFD";

    std::string serialized = builder.BuildPayload(
        "gw-12345-0000", "cam-01",
        "2026-05-08T10:30:00Z",
        binary_data, "jpeg");

    ::mould::gateway::ImageFrameIngress frame;
    ASSERT_TRUE(frame.ParseFromString(serialized));
    EXPECT_EQ(frame.image_data().size(), binary_data.size());
    EXPECT_EQ(frame.image_data(), binary_data);
}

TEST(HandlerFramePayloadTest, LargeImageData) {
    ImageFramePayloadBuilder builder;
    std::string large_data(1024 * 1024, 'A');  // 1 MB

    std::string serialized = builder.BuildPayload(
        "gw-12345-0002", "cam-01",
        "2026-05-08T10:30:00Z",
        large_data, "jpeg");

    ::mould::gateway::ImageFrameIngress frame;
    ASSERT_TRUE(frame.ParseFromString(serialized));
    EXPECT_EQ(frame.image_data().size(), 1024 * 1024);
}

TEST(HandlerFramePayloadTest, ParseFromInvalidData) {
    // 无效的 proto 字节应返回解析失败
    std::string garbage = "this is not proto data";
    ::mould::gateway::ImageFrameIngress frame;
    EXPECT_FALSE(frame.ParseFromString(garbage));
}

TEST(HandlerFramePayloadTest, EmptyFieldsRoundTrip) {
    ImageFramePayloadBuilder builder;
    // image_data 和 image_format 可能为空
    std::string serialized = builder.BuildPayload(
        "gw-12345-0000", "cam-01",
        "2026-05-08T10:30:00Z",
        "", "");

    ::mould::gateway::ImageFrameIngress frame;
    ASSERT_TRUE(frame.ParseFromString(serialized));
    EXPECT_TRUE(frame.image_data().empty());
    EXPECT_TRUE(frame.image_format().empty());
}

// ============================================================
// TokenBucketRateLimiter 测试
// ============================================================

TEST(HandlerRateLimiterTest, AcquireWithinBurstLimit) {
    TokenBucketRateLimiter limiter(100.0, 10.0);
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(limiter.Acquire()) << "burst consume #" << i;
    }
    // 第 11 次应该失败
    EXPECT_FALSE(limiter.Acquire());
}

TEST(HandlerRateLimiterTest, NoTokensAfterBurst) {
    TokenBucketRateLimiter limiter(100.0, 5.0);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(limiter.Acquire());
    }
    EXPECT_FALSE(limiter.Acquire());
}

TEST(HandlerRateLimiterTest, RefillOverTime) {
    TokenBucketRateLimiter limiter(100.0, 10.0);
    // 消耗全部令牌
    for (int i = 0; i < 10; ++i) {
        limiter.Acquire();
    }
    EXPECT_FALSE(limiter.Acquire());

    // 等待 50ms, 100 tokens/s = 0.1 tokens/ms, 50ms = 5 tokens
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_TRUE(limiter.Acquire());
}

TEST(HandlerRateLimiterTest, AcquireWithExplicitTimestamp) {
    TokenBucketRateLimiter limiter(1000.0, 5.0);  // 1000 tokens/s, burst=5
    int64_t now = 1000000;

    // 消耗全部 5 个令牌
    EXPECT_TRUE(limiter.Acquire(now));
    EXPECT_TRUE(limiter.Acquire(now));
    EXPECT_TRUE(limiter.Acquire(now));
    EXPECT_TRUE(limiter.Acquire(now));
    EXPECT_TRUE(limiter.Acquire(now));
    EXPECT_FALSE(limiter.Acquire(now));

    // 时间前进 10ms (1000 tokens/s = 1 token/ms, 10ms = 10 tokens, capped at burst=5)
    now += 10;
    EXPECT_TRUE(limiter.Acquire(now));
    EXPECT_TRUE(limiter.Acquire(now));
    EXPECT_TRUE(limiter.Acquire(now));
    EXPECT_TRUE(limiter.Acquire(now));
    EXPECT_TRUE(limiter.Acquire(now));
    EXPECT_FALSE(limiter.Acquire(now));
}

TEST(HandlerRateLimiterTest, TokensReturnsApproximateValue) {
    TokenBucketRateLimiter limiter(10.0, 100.0);
    double initial = limiter.Tokens();
    EXPECT_GE(initial, 99.0);  // 接近 burst

    limiter.Acquire();
    double after_one = limiter.Tokens();
    EXPECT_LT(after_one, initial);
}

TEST(HandlerRateLimiterTest, RateClampedToMinimum) {
    // rate=0 应被 clamp 到 1.0
    TokenBucketRateLimiter limiter(0.0, 5.0);
    EXPECT_TRUE(limiter.Acquire());
    EXPECT_TRUE(limiter.Acquire());
    EXPECT_TRUE(limiter.Acquire());
    EXPECT_TRUE(limiter.Acquire());
    EXPECT_TRUE(limiter.Acquire());
    EXPECT_FALSE(limiter.Acquire());  // burst 耗尽

    // 等待超过 1 秒，应补充 1 个令牌（rate clamped to 1.0）
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    EXPECT_TRUE(limiter.Acquire());
    EXPECT_FALSE(limiter.Acquire());  // 立刻又耗尽
}

TEST(HandlerRateLimiterTest, BurstClampedToMinimum) {
    // burst=0 应被 clamp 到 1.0
    TokenBucketRateLimiter limiter(10.0, 0.0);
    EXPECT_TRUE(limiter.Acquire());
    EXPECT_FALSE(limiter.Acquire());
}

TEST(HandlerRateLimiterTest, ResetReconfiguresLimiter) {
    TokenBucketRateLimiter limiter(10.0, 5.0);
    EXPECT_TRUE(limiter.Acquire());

    limiter.Reset(1000.0, 100.0);
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(limiter.Acquire()) << "consume after reset #" << i;
    }
    EXPECT_FALSE(limiter.Acquire());
}

TEST(HandlerRateLimiterTest, ThreadSafety) {
    TokenBucketRateLimiter limiter(10000.0, 10000.0);
    std::atomic<int> success_count{0};
    constexpr int kThreads = 4;
    constexpr int kCallsPerThread = 500;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < kCallsPerThread; ++i) {
                if (limiter.Acquire()) {
                    success_count++;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 总成功数不超过 burst
    EXPECT_LE(success_count.load(), 10000);
    EXPECT_GT(success_count.load(), 0);
}

// ============================================================
// LoggingMetricsRecorder 测试
// ============================================================

TEST(HandlerMetricsRecorderTest, RecordRequestIncrementsCounters) {
    LoggingMetricsRecorder recorder;

    RequestMetrics metrics;
    metrics.request_id = "gw-12345-0001";
    metrics.node_id = "camera-01";
    metrics.http_status_code = 200;
    metrics.latency_ms = 42;
    metrics.shm_publish_success = true;
    metrics.oss_result = "success";

    recorder.RecordRequest(metrics);

    EXPECT_EQ(recorder.TotalRequests(), 1);
    EXPECT_EQ(recorder.SuccessRequests(), 1);
    EXPECT_EQ(recorder.FailedRequests(), 0);
    EXPECT_EQ(recorder.TotalLatencyMs(), 42);
    EXPECT_DOUBLE_EQ(recorder.AvgLatencyMs(), 42.0);
}

TEST(HandlerMetricsRecorderTest, MultipleRecordsAccumulate) {
    LoggingMetricsRecorder recorder;

    for (int i = 0; i < 5; ++i) {
        RequestMetrics metrics;
        metrics.request_id = "gw-12345-000" + std::to_string(i);
        metrics.node_id = "camera-01";
        metrics.http_status_code = 200;
        metrics.latency_ms = 10;
        metrics.shm_publish_success = true;
        metrics.oss_result = "success";
        recorder.RecordRequest(metrics);
    }

    EXPECT_EQ(recorder.TotalRequests(), 5);
    EXPECT_EQ(recorder.SuccessRequests(), 5);
    EXPECT_EQ(recorder.FailedRequests(), 0);
    EXPECT_EQ(recorder.TotalLatencyMs(), 50);
    EXPECT_DOUBLE_EQ(recorder.AvgLatencyMs(), 10.0);
}

TEST(HandlerMetricsRecorderTest, FailedRequestTracking) {
    LoggingMetricsRecorder recorder;

    // 成功请求
    RequestMetrics success;
    success.request_id = "gw-12345-s1";
    success.http_status_code = 200;
    success.latency_ms = 30;
    success.shm_publish_success = true;
    success.oss_result = "success";
    recorder.RecordRequest(success);

    // 失败请求
    RequestMetrics failure;
    failure.request_id = "gw-12345-f1";
    failure.http_status_code = 500;
    failure.latency_ms = 100;
    failure.shm_publish_success = false;
    failure.oss_result = "failed";
    recorder.RecordRequest(failure);

    EXPECT_EQ(recorder.TotalRequests(), 2);
    EXPECT_EQ(recorder.SuccessRequests(), 1);
    EXPECT_EQ(recorder.FailedRequests(), 1);
    // 成功 = 2xx, 失败 = 非 2xx
    EXPECT_EQ(recorder.TotalLatencyMs(), 130);
    EXPECT_DOUBLE_EQ(recorder.AvgLatencyMs(), 65.0);
}

TEST(HandlerMetricsRecorderTest, ZeroRequestsAvgLatency) {
    LoggingMetricsRecorder recorder;
    EXPECT_DOUBLE_EQ(recorder.AvgLatencyMs(), 0.0);
}

TEST(HandlerMetricsRecorderTest, HttpStatusBoundaries) {
    LoggingMetricsRecorder recorder;

    // 3xx 应被计为失败
    RequestMetrics redirect;
    redirect.http_status_code = 301;
    recorder.RecordRequest(redirect);

    EXPECT_EQ(recorder.TotalRequests(), 1);
    EXPECT_EQ(recorder.SuccessRequests(), 0);
    EXPECT_EQ(recorder.FailedRequests(), 1);

    // 2xx 应被计为成功
    RequestMetrics ok;
    ok.http_status_code = 201;
    recorder.RecordRequest(ok);

    EXPECT_EQ(recorder.TotalRequests(), 2);
    EXPECT_EQ(recorder.SuccessRequests(), 1);
    EXPECT_EQ(recorder.FailedRequests(), 1);
}

TEST(HandlerMetricsRecorderTest, OssResultVariations) {
    LoggingMetricsRecorder recorder;

    auto record = [&](const std::string& oss_result) {
        RequestMetrics m;
        m.http_status_code = 200;
        m.oss_result = oss_result;
        recorder.RecordRequest(m);
    };

    record("success");
    record("failed");
    record("skipped");

    EXPECT_EQ(recorder.TotalRequests(), 3);
    EXPECT_EQ(recorder.SuccessRequests(), 3);  // 状态码都是 2xx
}

// ============================================================
// UUID 生成测试
// ============================================================

TEST(HandlerUuidTest, FormatMatchesPattern) {
    std::string id = GenerateRequestId();
    // 格式: gw-{timestamp}-{hex}
    EXPECT_NE(id.find("gw-"), std::string::npos);
    EXPECT_EQ(id.substr(0, 3), "gw-");
    // 至少有 "gw-X-" 三个部分
    size_t first_dash = id.find('-');
    size_t second_dash = id.find('-', first_dash + 1);
    EXPECT_NE(first_dash, std::string::npos);
    EXPECT_NE(second_dash, std::string::npos);
    // 时间戳部分应为数字
    std::string ts_part = id.substr(first_dash + 1, second_dash - first_dash - 1);
    EXPECT_FALSE(ts_part.empty());
    for (char c : ts_part) {
        EXPECT_TRUE(c >= '0' && c <= '9') << "Non-digit in timestamp: " << c;
    }
    // hex 部分应非空
    std::string hex_part = id.substr(second_dash + 1);
    EXPECT_FALSE(hex_part.empty());
}

TEST(HandlerUuidTest, UniqueIds) {
    constexpr int kCount = 1000;
    std::vector<std::string> ids;
    ids.reserve(kCount);

    for (int i = 0; i < kCount; ++i) {
        ids.push_back(GenerateRequestId());
    }

    // 检查是否全部唯一
    std::sort(ids.begin(), ids.end());
    for (int i = 1; i < kCount; ++i) {
        EXPECT_NE(ids[i], ids[i - 1]) << "Duplicate ID at index " << i;
    }
}

TEST(HandlerUuidTest, SequentialIdsIncreasing) {
    // 连续生成的 ID 应包含单调递增的时间戳或序列号
    std::string prev = GenerateRequestId();
    for (int i = 0; i < 100; ++i) {
        std::string cur = GenerateRequestId();
        // 字符串排序应保持顺序（因为时间戳在前）
        EXPECT_GE(cur, prev);
        prev = cur;
    }
}

TEST(HandlerUuidTest, ThreadSafety) {
    constexpr int kThreads = 4;
    constexpr int kIdsPerThread = 500;
    std::vector<std::string> all_ids;
    std::mutex ids_mutex;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&]() {
            std::vector<std::string> local_ids;
            for (int i = 0; i < kIdsPerThread; ++i) {
                local_ids.push_back(GenerateRequestId());
            }
            std::lock_guard<std::mutex> lock(ids_mutex);
            all_ids.insert(all_ids.end(), local_ids.begin(), local_ids.end());
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 检查是否有重复
    EXPECT_EQ(all_ids.size(), kThreads * kIdsPerThread);
    std::sort(all_ids.begin(), all_ids.end());
    for (size_t i = 1; i < all_ids.size(); ++i) {
        EXPECT_NE(all_ids[i], all_ids[i - 1])
            << "Duplicate ID across threads at index " << i;
    }
}

TEST(HandlerUuidTest, ConsistentPrefix) {
    for (int i = 0; i < 10; ++i) {
        std::string id = GenerateRequestId();
        EXPECT_EQ(id.substr(0, 3), "gw-");
    }
}

// ============================================================
// FakeShmFramePublisher 测试
// ============================================================

TEST(HandlerShmPublisherTest, FakePublisherRecordsCalls) {
    FakeShmFramePublisher publisher;

    ShmPublishResult success_result;
    success_result.success = true;
    success_result.image_id = "img-001";
    publisher.result_to_return_ = success_result;

    ShmPublishResult r = publisher.Publish("frame_channel", "payload_data");

    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.image_id, "img-001");
    EXPECT_EQ(publisher.published_count_, 1);
    EXPECT_EQ(publisher.last_channel_, "frame_channel");
    EXPECT_EQ(publisher.last_payload_, "payload_data");
}

TEST(HandlerShmPublisherTest, FakePublisherReturnsConfiguredFailure) {
    FakeShmFramePublisher publisher;

    ShmPublishResult fail_result;
    fail_result.success = false;
    fail_result.error_type = ShmPublishResult::ErrorType::kBackpressure;
    fail_result.error_message = "Backpressure detected";
    publisher.result_to_return_ = fail_result;

    ShmPublishResult r = publisher.Publish("ch", "data");

    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error_type, ShmPublishResult::ErrorType::kBackpressure);
    EXPECT_EQ(r.error_message, "Backpressure detected");
    EXPECT_EQ(publisher.published_count_, 1);
}

TEST(HandlerShmPublisherTest, FakePublisherMultipleCalls) {
    FakeShmFramePublisher publisher;

    ShmPublishResult ok;
    ok.success = true;
    publisher.result_to_return_ = ok;

    publisher.Publish("ch1", "payload1");
    publisher.Publish("ch2", "payload2");
    publisher.Publish("ch1", "payload3");

    EXPECT_EQ(publisher.published_count_, 3);
    EXPECT_EQ(publisher.last_channel_, "ch1");
    EXPECT_EQ(publisher.last_payload_, "payload3");
}

// ============================================================
// IngestHandler 测试辅助
// ============================================================

/// 构建有效的 JPEG 原始图片数据
static std::string BuildValidJpegData() {
    std::string data;
    data += static_cast<char>(0xFF);
    data += static_cast<char>(0xD8);
    data += static_cast<char>(0xFF);
    data += static_cast<char>(0xE0);
    data += std::string(50, 'x');
    return data;
}

/// 构建一个带默认依赖的 IngestHandler，方便测试
static std::unique_ptr<IngestHandler> CreateTestHandler(
    std::unique_ptr<IShmFramePublisher> shm_pub,
    std::unique_ptr<oss::IBlobStorageUploader> oss_uploader) {

    auto validator = std::make_unique<RequestValidator>();
    auto payload_builder = std::make_unique<ImageFramePayloadBuilder>();
    auto rate_limiter = std::make_unique<TokenBucketRateLimiter>(10000.0, 10000.0);
    auto metrics = std::make_unique<LoggingMetricsRecorder>();
    auto path_policy = std::make_unique<oss::DefaultUploadPathPolicy>();

    return std::make_unique<IngestHandler>(
        std::move(validator),
        std::move(payload_builder),
        std::move(shm_pub),
        std::move(oss_uploader),
        std::move(rate_limiter),
        std::move(metrics),
        std::move(path_policy));
}

/// 创建 RequestView，元数据通过 HTTP Headers 设置
static http::RequestView MakeView(
    const std::string& body,
    const std::string& content_type,
    const std::string& client_ip,
    const std::string& request_id = "test-req") {
    http::RequestView view;
    view.body = body;
    view.content_type = content_type;
    view.client_ip = client_ip;
    view.request_id = request_id;
    // 默认标准的元数据头
    view.headers["content-type"] = content_type;
    view.headers["x-node-id"] = "camera-01";
    view.headers["x-capture-time"] = "2026-05-08T10:30:00Z";
    return view;
}

// ============================================================
// IngestHandler 正常流程测试
// ============================================================

TEST(HandlerIngestTest, HappyPathReturns200) {
    auto shm_pub = std::make_unique<FakeShmFramePublisher>();
    ShmPublishResult shm_ok;
    shm_ok.success = true;
    shm_ok.image_id = "shm-img-001";
    shm_pub->result_to_return_ = shm_ok;

    auto oss_uploader = std::make_unique<oss::NoOpUploader>();

    auto handler = CreateTestHandler(std::move(shm_pub), std::move(oss_uploader));
    auto view = MakeView(BuildValidJpegData(), "image/jpeg", "192.168.1.100");
    auto result = handler->Handle(view);

    ASSERT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.http_status_code, 200);
    EXPECT_EQ(result.request_id, "test-req");
    EXPECT_EQ(result.resource_id, "shm-img-001");
}

TEST(HandlerIngestTest, HappyPathPropagatesRequestId) {
    auto shm_pub = std::make_unique<FakeShmFramePublisher>();
    ShmPublishResult shm_ok;
    shm_ok.success = true;
    shm_pub->result_to_return_ = shm_ok;

    auto oss_uploader = std::make_unique<oss::NoOpUploader>();

    auto handler = CreateTestHandler(std::move(shm_pub), std::move(oss_uploader));
    auto view = MakeView(BuildValidJpegData(), "image/jpeg", "10.0.0.1", "custom-req-id");
    auto result = handler->Handle(view);

    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.request_id, "custom-req-id");
}

TEST(HandlerIngestTest, HappyPathFakeShmRecordsCall) {
    auto shm_pub = std::make_unique<FakeShmFramePublisher>();
    ShmPublishResult shm_ok;
    shm_ok.success = true;
    shm_ok.image_id = "img-42";
    shm_pub->result_to_return_ = shm_ok;
    FakeShmFramePublisher* raw_pub = shm_pub.get();

    auto oss_uploader = std::make_unique<oss::NoOpUploader>();

    auto handler = CreateTestHandler(std::move(shm_pub), std::move(oss_uploader));
    auto view = MakeView(BuildValidJpegData(), "image/jpeg", "10.0.0.1");
    handler->Handle(view);

    EXPECT_EQ(raw_pub->published_count_, 1);
    EXPECT_EQ(raw_pub->last_channel_, "ImageFrameIngress");
    EXPECT_FALSE(raw_pub->last_payload_.empty());
}

// ============================================================
// IngestHandler X-Mould-* 元数据头测试
// ============================================================

TEST(HandlerIngestTest, ExtraMetadataFromHeaders) {
    auto shm_pub = std::make_unique<FakeShmFramePublisher>();
    ShmPublishResult shm_ok;
    shm_ok.success = true;
    shm_ok.image_id = "img-extra";
    shm_pub->result_to_return_ = shm_ok;
    FakeShmFramePublisher* raw_pub = shm_pub.get();

    auto oss_uploader = std::make_unique<oss::NoOpUploader>();

    auto handler = CreateTestHandler(std::move(shm_pub), std::move(oss_uploader));
    auto view = MakeView(BuildValidJpegData(), "image/jpeg", "10.0.0.1");
    view.headers["x-mould-batch-id"] = "batch-20260508";
    view.headers["x-mould-product-type"] = "type-a";
    handler->Handle(view);

    // 验证 proto 中包含 extra_metadata
    ::mould::gateway::ImageFrameIngress frame;
    ASSERT_TRUE(frame.ParseFromString(raw_pub->last_payload_));
    EXPECT_EQ(frame.extra_metadata().at("batch-id"), "batch-20260508");
    EXPECT_EQ(frame.extra_metadata().at("product-type"), "type-a");
    EXPECT_EQ(frame.extra_metadata_size(), 2);
}

// ============================================================
// IngestHandler 校验阶段失败测试
// ============================================================

TEST(HandlerIngestTest, MissingNodeIdReturns422) {
    auto shm_pub = std::make_unique<FakeShmFramePublisher>();
    auto oss_uploader = std::make_unique<oss::NoOpUploader>();

    auto handler = CreateTestHandler(std::move(shm_pub), std::move(oss_uploader));

    auto view = MakeView(BuildValidJpegData(), "image/jpeg", "10.0.0.1");
    view.headers.erase("x-node-id");
    auto result = handler->Handle(view);

    ASSERT_FALSE(result.success);
    EXPECT_EQ(result.http_status_code, 422);
    EXPECT_FALSE(result.error_message.empty());
}

TEST(HandlerIngestTest, MissingCaptureTimeReturns422) {
    auto shm_pub = std::make_unique<FakeShmFramePublisher>();
    auto oss_uploader = std::make_unique<oss::NoOpUploader>();

    auto handler = CreateTestHandler(std::move(shm_pub), std::move(oss_uploader));

    auto view = MakeView(BuildValidJpegData(), "image/jpeg", "10.0.0.1");
    view.headers.erase("x-capture-time");
    auto result = handler->Handle(view);

    ASSERT_FALSE(result.success);
    EXPECT_EQ(result.http_status_code, 422);
}

TEST(HandlerIngestTest, InvalidImageFormatReturns415) {
    auto shm_pub = std::make_unique<FakeShmFramePublisher>();
    auto oss_uploader = std::make_unique<oss::NoOpUploader>();

    auto handler = CreateTestHandler(std::move(shm_pub), std::move(oss_uploader));

    // 声明为 jpeg 但 body 是无效数据
    std::string bad_image_data = "not_an_image_at_all";
    auto view = MakeView(bad_image_data, "image/jpeg", "10.0.0.1");
    auto result = handler->Handle(view);

    ASSERT_FALSE(result.success);
    EXPECT_EQ(result.http_status_code, 415);
}

TEST(HandlerIngestTest, EmptyBodyReturns400) {
    auto shm_pub = std::make_unique<FakeShmFramePublisher>();
    auto oss_uploader = std::make_unique<oss::NoOpUploader>();

    auto handler = CreateTestHandler(std::move(shm_pub), std::move(oss_uploader));

    auto view = MakeView("", "image/jpeg", "10.0.0.1");
    auto result = handler->Handle(view);

    ASSERT_FALSE(result.success);
    EXPECT_EQ(result.http_status_code, 422);  // image_data 为空
    EXPECT_FALSE(result.error_message.empty());
}

// ============================================================
// IngestHandler SHM Publish 失败测试
// ============================================================

TEST(HandlerIngestTest, ShmBackpressureReturns503) {
    auto shm_pub = std::make_unique<FakeShmFramePublisher>();
    ShmPublishResult shm_fail;
    shm_fail.success = false;
    shm_fail.error_type = ShmPublishResult::ErrorType::kBackpressure;
    shm_fail.error_message = "Backpressure on SHM channel";
    shm_pub->result_to_return_ = shm_fail;

    auto oss_uploader = std::make_unique<oss::NoOpUploader>();

    auto handler = CreateTestHandler(std::move(shm_pub), std::move(oss_uploader));
    auto view = MakeView(BuildValidJpegData(), "image/jpeg", "10.0.0.1");
    auto result = handler->Handle(view);

    ASSERT_FALSE(result.success);
    EXPECT_EQ(result.http_status_code, 503);
    EXPECT_EQ(result.error_message, "Backpressure on SHM channel");
}

TEST(HandlerIngestTest, ShmSlotFullReturns503) {
    auto shm_pub = std::make_unique<FakeShmFramePublisher>();
    ShmPublishResult shm_fail;
    shm_fail.success = false;
    shm_fail.error_type = ShmPublishResult::ErrorType::kSlotFull;
    shm_fail.error_message = "SHM slot full";
    shm_pub->result_to_return_ = shm_fail;

    auto oss_uploader = std::make_unique<oss::NoOpUploader>();

    auto handler = CreateTestHandler(std::move(shm_pub), std::move(oss_uploader));
    auto view = MakeView(BuildValidJpegData(), "image/jpeg", "10.0.0.1");
    auto result = handler->Handle(view);

    ASSERT_FALSE(result.success);
    EXPECT_EQ(result.http_status_code, 503);
}

TEST(HandlerIngestTest, ShmOtherErrorReturns503) {
    auto shm_pub = std::make_unique<FakeShmFramePublisher>();
    ShmPublishResult shm_fail;
    shm_fail.success = false;
    shm_fail.error_type = ShmPublishResult::ErrorType::kOther;
    shm_fail.error_message = "Unknown SHM error";
    shm_pub->result_to_return_ = shm_fail;

    auto oss_uploader = std::make_unique<oss::NoOpUploader>();

    auto handler = CreateTestHandler(std::move(shm_pub), std::move(oss_uploader));
    auto view = MakeView(BuildValidJpegData(), "image/jpeg", "10.0.0.1");
    auto result = handler->Handle(view);

    ASSERT_FALSE(result.success);
    EXPECT_EQ(result.http_status_code, 503);
}

// ============================================================
// IngestHandler OSS 失败不影响 HTTP 200 测试
// ============================================================

namespace {

/// 模拟 OSS 上传失败的 Uploader（同步回调失败）
class FailingUploader : public oss::IBlobStorageUploader {
public:
    void UploadAsync(
        const std::string& /*image_data*/,
        const std::unordered_map<std::string, std::string>& /*metadata*/,
        UploadCallback callback) override {
        UploadResult result;
        result.success = false;
        result.error_code = "UPLOAD_FAILED";
        result.error_message = "Simulated OSS upload failure";
        if (callback) {
            callback(result);
        }
    }
};

}  // namespace

TEST(HandlerIngestTest, OssFailureDoesNotAffectHttpStatus) {
    auto shm_pub = std::make_unique<FakeShmFramePublisher>();
    ShmPublishResult shm_ok;
    shm_ok.success = true;
    shm_ok.image_id = "img-oss-fail-test";
    shm_pub->result_to_return_ = shm_ok;

    auto failing_uploader = std::make_unique<FailingUploader>();

    auto handler = CreateTestHandler(std::move(shm_pub), std::move(failing_uploader));
    auto view = MakeView(BuildValidJpegData(), "image/jpeg", "10.0.0.1");
    auto result = handler->Handle(view);

    // OSS 失败不应影响 HTTP 响应码
    ASSERT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(result.http_status_code, 200);
    EXPECT_EQ(result.resource_id, "img-oss-fail-test");
}

TEST(HandlerIngestTest, OssFailureDoesNotPreventShmPublish) {
    auto shm_pub = std::make_unique<FakeShmFramePublisher>();
    ShmPublishResult shm_ok;
    shm_ok.success = true;
    shm_ok.image_id = "img-002";
    shm_pub->result_to_return_ = shm_ok;
    FakeShmFramePublisher* raw_pub = shm_pub.get();

    auto failing_uploader = std::make_unique<FailingUploader>();

    auto handler = CreateTestHandler(std::move(shm_pub), std::move(failing_uploader));
    auto view = MakeView(BuildValidJpegData(), "image/jpeg", "10.0.0.1");
    handler->Handle(view);

    // SHM publish 仍然执行了
    EXPECT_EQ(raw_pub->published_count_, 1);
}

}  // namespace
}  // namespace handler
}  // namespace gate
}  // namespace mould

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
