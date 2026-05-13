#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mould {
namespace gate {
namespace handler {

/// 请求校验结果
struct ValidationResult {
    bool valid = false;
    std::vector<std::string> missing_fields;
    std::vector<std::string> invalid_fields;
    std::string image_format_error;
    int http_status_code = 422;  // 默认 422 Unprocessable Entity
    std::string error_message;
};

/// 请求校验器，校验请求必填字段、格式和图片 magic bytes
class RequestValidator {
public:
    explicit RequestValidator(size_t max_field_length = 256)
        : max_field_length_(max_field_length) {}

    /// 校验必填字段、字段格式和图片数据
    ValidationResult Validate(const std::string& node_id,
                              const std::string& capture_time,
                              const std::string& image_data,
                              const std::string& image_format) const;

    /// 校验单个字段长度（默认最大 256 字符）
    bool ValidateFieldLength(const std::string& field_name,
                             const std::string& value) const;

    /// 校验 capture_time 的 ISO 8601 格式
    bool ValidateCaptureTimeFormat(const std::string& capture_time) const;

    /// 校验图片 magic bytes
    /// 成功返回空字符串，失败返回错误描述
    std::string ValidateImageMagicBytes(const std::string& image_data,
                                        const std::string& image_format) const;

private:
    size_t max_field_length_;
};

// ============================================================
// 实现
// ============================================================

inline ValidationResult RequestValidator::Validate(
    const std::string& node_id,
    const std::string& capture_time,
    const std::string& image_data,
    const std::string& image_format) const {

    ValidationResult result;

    // 1. 必填字段校验
    if (node_id.empty()) {
        result.missing_fields.push_back("node_id");
    }
    if (capture_time.empty()) {
        result.missing_fields.push_back("capture_time");
    }
    if (image_data.empty()) {
        result.missing_fields.push_back("image_data");
    }

    if (!result.missing_fields.empty()) {
        result.valid = false;
        result.http_status_code = 422;
        result.error_message = "Missing required fields";
        return result;
    }

    // 2. 字段长度校验
    bool field_length_ok = true;
    if (!ValidateFieldLength("node_id", node_id)) {
        result.invalid_fields.push_back("node_id");
        field_length_ok = false;
    }
    if (!ValidateFieldLength("capture_time", capture_time)) {
        result.invalid_fields.push_back("capture_time");
        field_length_ok = false;
    }

    if (!field_length_ok) {
        result.valid = false;
        result.http_status_code = 422;
        result.error_message = "Field length exceeds maximum allowed";
        return result;
    }

    // 3. capture_time 格式校验
    if (!ValidateCaptureTimeFormat(capture_time)) {
        result.invalid_fields.push_back("capture_time");
        result.valid = false;
        result.http_status_code = 422;
        result.error_message = "Invalid capture_time format, expected ISO 8601";
        return result;
    }

    // 4. 图片格式校验
    if (!image_format.empty()) {
        std::string magic_error = ValidateImageMagicBytes(image_data, image_format);
        if (!magic_error.empty()) {
            result.image_format_error = magic_error;
            result.invalid_fields.push_back("image_data");
            result.valid = false;
            result.http_status_code = 415;  // Unsupported Media Type
            result.error_message = magic_error;
            return result;
        }
    }

    result.valid = true;
    result.http_status_code = 200;
    return result;
}

inline bool RequestValidator::ValidateFieldLength(
    const std::string& field_name,
    const std::string& value) const {
    (void)field_name;
    return value.size() <= max_field_length_;
}

inline bool RequestValidator::ValidateCaptureTimeFormat(
    const std::string& capture_time) const {
    // ISO 8601 格式: 2026-05-08T10:30:00Z
    // 支持的形式:
    //   yyyy-MM-ddTHH:mm:ssZ
    //   yyyy-MM-ddTHH:mm:ss±HH:mm
    //   yyyy-MM-ddTHH:mm:ss.sssZ
    if (capture_time.size() < 20) {
        return false;
    }

    // 基本格式检查: yyyy-MM-ddTHH:mm:ss
    if (capture_time[4] != '-' || capture_time[7] != '-' ||
        capture_time[10] != 'T' || capture_time[13] != ':' ||
        capture_time[16] != ':') {
        return false;
    }

    // 检查日期数字部分
    auto check_digit = [](char c) { return c >= '0' && c <= '9'; };
    for (int i = 0; i <= 3; ++i) {
        if (!check_digit(capture_time[i])) return false;  // 年份
    }
    if (!check_digit(capture_time[5]) || !check_digit(capture_time[6])) {
        return false;  // 月份
    }
    if (!check_digit(capture_time[8]) || !check_digit(capture_time[9])) {
        return false;  // 日
    }
    if (!check_digit(capture_time[11]) || !check_digit(capture_time[12])) {
        return false;  // 时
    }
    if (!check_digit(capture_time[14]) || !check_digit(capture_time[15])) {
        return false;  // 分
    }
    if (!check_digit(capture_time[17]) || !check_digit(capture_time[18])) {
        return false;  // 秒
    }

    size_t pos = 19;

    // 可选的小数秒
    if (pos < capture_time.size() && capture_time[pos] == '.') {
        pos++;
        while (pos < capture_time.size() &&
               check_digit(capture_time[pos])) {
            pos++;
        }
    }

    // 时区部分: Z 或 ±HH:MM
    if (pos >= capture_time.size()) {
        return false;
    }

    if (capture_time[pos] == 'Z') {
        pos++;
    } else if (capture_time[pos] == '+' || capture_time[pos] == '-') {
        pos++;
        if (pos + 5 > capture_time.size()) return false;
        if (!check_digit(capture_time[pos]) ||
            !check_digit(capture_time[pos + 1])) {
            return false;
        }
        if (capture_time[pos + 2] != ':') return false;
        if (!check_digit(capture_time[pos + 3]) ||
            !check_digit(capture_time[pos + 4])) {
            return false;
        }
        pos += 5;
    } else {
        return false;
    }

    // 确保没有多余字符
    return pos == capture_time.size();
}

inline std::string RequestValidator::ValidateImageMagicBytes(
    const std::string& image_data,
    const std::string& image_format) const {

    if (image_data.empty()) {
        return "Empty image data";
    }

    bool magic_ok = false;
    std::string detected_format;

    if (image_data.size() >= 3 &&
        static_cast<uint8_t>(image_data[0]) == 0xFF &&
        static_cast<uint8_t>(image_data[1]) == 0xD8 &&
        static_cast<uint8_t>(image_data[2]) == 0xFF) {
        detected_format = "jpeg";
        magic_ok = true;
    } else if (image_data.size() >= 4 &&
               static_cast<uint8_t>(image_data[0]) == 0x89 &&
               static_cast<uint8_t>(image_data[1]) == 'P' &&
               static_cast<uint8_t>(image_data[2]) == 'N' &&
               static_cast<uint8_t>(image_data[3]) == 'G') {
        detected_format = "png";
        magic_ok = true;
    } else if (image_data.size() >= 2 &&
               static_cast<uint8_t>(image_data[0]) == 0x42 &&
               static_cast<uint8_t>(image_data[1]) == 0x4D) {
        detected_format = "bmp";
        magic_ok = true;
    }

    if (!magic_ok) {
        return "Unrecognized image format: magic bytes do not match JPEG, PNG, or BMP";
    }

    if (detected_format != image_format) {
        return "Image format mismatch: declared '" + image_format +
               "' but magic bytes indicate '" + detected_format + "'";
    }

    return "";
}

}  // namespace handler
}  // namespace gate
}  // namespace mould
