#pragma once

#include <cstdint>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace mould {
namespace gate {
namespace handler {

/// 解析结果
struct ParseResult {
    bool success = false;
    std::string error_message;
    std::string image_data;                                           // 解析出的图片二进制
    std::string image_format;                                         // jpeg/png/bmp
    std::string image_filename;                                       // 原始文件名（来自 Content-Disposition）
    std::unordered_map<std::string, std::string> fields;              // 元数据字段
};

/// 请求体解析器接口
class IRequestBodyParser {
public:
    virtual ~IRequestBodyParser() = default;
    virtual ParseResult Parse(const std::string& body,
                              const std::string& content_type) = 0;
};

/// Multipart/form-data 解析器实现
///
/// 从 Content-Type 提取 boundary，按 boundary 切分各部分，
/// 识别 name="image" 的文件字段和 name="node_id"、name="capture_time" 等文本字段。
class MultipartFormDataParser : public IRequestBodyParser {
public:
    ParseResult Parse(const std::string& body,
                      const std::string& content_type) override;

private:
    /// 从 Content-Type 头中提取 boundary 值
    std::string ExtractBoundary(const std::string& content_type);

    /// 将 body 按 boundary 切分为各部分（包含头部和正文）
    std::vector<std::string> SplitByBoundary(const std::string& body,
                                             const std::string& boundary);

    /// 解析单个部分的头部，返回头部映射和正文起始位置
    struct PartHeaders {
        std::unordered_map<std::string, std::string> headers;
        std::unordered_map<std::string, std::string> disposition_params;
        size_t body_start = 0;
    };
    PartHeaders ParsePartHeaders(const std::string& part);

    /// 从 Content-Disposition 中提取参数（如 name、filename）
    std::unordered_map<std::string, std::string> ParseDispositionParams(
        const std::string& disposition);

    /// 根据 Content-Type 映射图片格式
    std::string MapImageFormat(const std::string& content_type_value);

    /// 从 Content-Disposition 中提取 name 属性值
    std::string ExtractNameFromDisposition(
        const std::unordered_map<std::string, std::string>& params);
};

// ============================================================
// 实现
// ============================================================

inline ParseResult MultipartFormDataParser::Parse(
    const std::string& body, const std::string& content_type) {
    ParseResult result;

    if (body.empty()) {
        result.success = false;
        result.error_message = "Empty request body";
        return result;
    }

    std::string boundary = ExtractBoundary(content_type);
    if (boundary.empty()) {
        result.success = false;
        result.error_message = "No multipart boundary found in Content-Type";
        return result;
    }

    // RFC 2046: boundary 前面加 "--" 作为分隔符
    std::string delimiter = "--" + boundary;

    std::vector<std::string> parts = SplitByBoundary(body, delimiter);
    if (parts.empty()) {
        result.success = false;
        result.error_message = "No multipart parts found";
        return result;
    }

    bool image_found = false;

    for (const auto& part : parts) {
        if (part.empty()) {
            continue;
        }

        PartHeaders ph = ParsePartHeaders(part);
        if (ph.body_start >= part.size()) {
            continue;
        }

        std::string part_body = part.substr(ph.body_start);
        // 去掉尾部 \r\n（若有）
        while (!part_body.empty() &&
               (part_body.back() == '\r' || part_body.back() == '\n')) {
            part_body.pop_back();
        }

        std::string field_name = ExtractNameFromDisposition(ph.disposition_params);
        if (field_name.empty()) {
            continue;
        }

        // 检查是否为图片字段
        if (field_name == "image") {
            result.image_data = part_body;
            image_found = true;

            // 从 Content-Disposition 中提取原始文件名
            auto fn_it = ph.disposition_params.find("filename");
            if (fn_it != ph.disposition_params.end()) {
                result.image_filename = fn_it->second;
            }

            // 从 Content-Type 判断图片格式
            auto it = ph.headers.find("content-type");
            if (it != ph.headers.end() && !it->second.empty()) {
                result.image_format = MapImageFormat(it->second);
            } else {
                // 通过 magic bytes 自动检测
                if (part_body.size() >= 3 &&
                    static_cast<uint8_t>(part_body[0]) == 0xFF &&
                    static_cast<uint8_t>(part_body[1]) == 0xD8 &&
                    static_cast<uint8_t>(part_body[2]) == 0xFF) {
                    result.image_format = "jpeg";
                } else if (part_body.size() >= 4 &&
                           static_cast<uint8_t>(part_body[0]) == 0x89 &&
                           part_body[1] == 'P' && part_body[2] == 'N' &&
                           part_body[3] == 'G') {
                    result.image_format = "png";
                } else if (part_body.size() >= 2 &&
                           static_cast<uint8_t>(part_body[0]) == 0x42 &&
                           static_cast<uint8_t>(part_body[1]) == 0x4D) {
                    result.image_format = "bmp";
                } else {
                    result.image_format = "unknown";
                }
            }
        } else {
            // 文本字段
            result.fields[field_name] = part_body;
        }
    }

    if (!image_found) {
        result.success = false;
        result.error_message = "Missing 'image' field in multipart data";
        return result;
    }

    result.success = true;
    return result;
}

inline std::string MultipartFormDataParser::ExtractBoundary(
    const std::string& content_type) {
    // Content-Type: multipart/form-data; boundary=----xxxx
    static const std::string boundary_key = "boundary=";

    auto pos = content_type.find(boundary_key);
    if (pos == std::string::npos) {
        return "";
    }

    pos += boundary_key.size();
    auto end = content_type.find(';', pos);
    if (end == std::string::npos) {
        end = content_type.size();
    }

    std::string boundary = content_type.substr(pos, end - pos);
    // 去掉首尾空白
    while (!boundary.empty() && boundary.front() == ' ') {
        boundary.erase(0, 1);
    }
    while (!boundary.empty() && boundary.back() == ' ') {
        boundary.pop_back();
    }
    // 去掉首尾引号
    if (!boundary.empty() && boundary.front() == '"') {
        boundary.erase(0, 1);
    }
    if (!boundary.empty() && boundary.back() == '"') {
        boundary.pop_back();
    }

    return boundary;
}

inline std::vector<std::string> MultipartFormDataParser::SplitByBoundary(
    const std::string& body, const std::string& delimiter) {
    std::vector<std::string> parts;

    size_t start = 0;
    while (true) {
        size_t pos = body.find(delimiter, start);
        if (pos == std::string::npos) {
            break;
        }

        // 跳过分隔符本身
        size_t part_start = pos + delimiter.size();

        // 检查是否为结束分隔符（末尾有 "--"）
        if (part_start < body.size() && body[part_start] == '-') {
            break;
        }

        // 跳过 \r\n
        if (part_start < body.size() && body[part_start] == '\r') {
            part_start++;
        }
        if (part_start < body.size() && body[part_start] == '\n') {
            part_start++;
        }

        // 提取 part 内容（到下一个分隔符）
        size_t next_delim = body.find(delimiter, part_start);
        std::string part;
        if (next_delim == std::string::npos) {
            part = body.substr(part_start);
            start = body.size();
        } else {
            part = body.substr(part_start, next_delim - part_start);
            start = next_delim;
        }

        // 去掉尾部的 \r\n
        while (!part.empty() &&
               (part.back() == '\r' || part.back() == '\n')) {
            part.pop_back();
        }

        if (!part.empty()) {
            parts.push_back(std::move(part));
        }

        if (next_delim == std::string::npos) {
            break;
        }
    }

    return parts;
}

inline MultipartFormDataParser::PartHeaders
MultipartFormDataParser::ParsePartHeaders(const std::string& part) {
    PartHeaders result;

    // 头部和正文以 \r\n\r\n 分隔
    size_t header_end = part.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        header_end = part.find("\n\n");
        if (header_end == std::string::npos) {
            result.body_start = 0;
            return result;
        }
        result.body_start = header_end + 2;
    } else {
        result.body_start = header_end + 4;
    }

    std::string header_section = part.substr(0, header_end);
    std::istringstream stream(header_section);
    std::string line;

    while (std::getline(stream, line)) {
        // 去掉 \r
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        size_t colon_pos = line.find(':');
        if (colon_pos == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, colon_pos);
        std::string value;
        if (colon_pos + 1 < line.size()) {
            value = line.substr(colon_pos + 1);
            // 去掉前导空白
            while (!value.empty() && value.front() == ' ') {
                value.erase(0, 1);
            }
        }

        // 转小写存储
        for (auto& c : key) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        result.headers[key] = value;

        if (key == "content-disposition") {
            result.disposition_params = ParseDispositionParams(value);
        }

        if (key == "content-type") {
            // 已存入 headers
        }
    }

    return result;
}

inline std::unordered_map<std::string, std::string>
MultipartFormDataParser::ParseDispositionParams(
    const std::string& disposition) {
    std::unordered_map<std::string, std::string> params;

    // form-data; name="image"; filename="test.jpg"
    // 跳过第一段 (form-data)
    size_t pos = 0;

    while (pos < disposition.size()) {
        // 找 '=' 号
        size_t eq_pos = disposition.find('=', pos);
        if (eq_pos == std::string::npos) {
            break;
        }

        // key 是从上一个分隔到 '=' 的部分（去掉空白）
        size_t key_start = disposition.rfind(';', eq_pos);
        if (key_start == std::string::npos) {
            key_start = 0;
        } else {
            key_start++;  // 跳过 ';'
        }

        std::string key = disposition.substr(key_start, eq_pos - key_start);
        // 去掉空白
        while (!key.empty() && key.front() == ' ') {
            key.erase(0, 1);
        }

        // value 从 '=' 后开始，到 ';' 或结尾
        size_t val_start = eq_pos + 1;
        size_t val_end = disposition.find(';', val_start);
        if (val_end == std::string::npos) {
            val_end = disposition.size();
        }

        std::string value = disposition.substr(val_start, val_end - val_start);
        // 去掉空白
        while (!value.empty() && value.front() == ' ') {
            value.erase(0, 1);
        }
        // 去掉引号
        if (!value.empty() && value.front() == '"') {
            value.erase(0, 1);
        }
        if (!value.empty() && value.back() == '"') {
            value.pop_back();
        }

        if (!key.empty()) {
            params[key] = value;
        }

        pos = val_end;
    }

    return params;
}

inline std::string MultipartFormDataParser::MapImageFormat(
    const std::string& content_type_value) {
    if (content_type_value.find("image/jpeg") != std::string::npos ||
        content_type_value.find("image/jpg") != std::string::npos) {
        return "jpeg";
    }
    if (content_type_value.find("image/png") != std::string::npos) {
        return "png";
    }
    if (content_type_value.find("image/bmp") != std::string::npos ||
        content_type_value.find("image/x-ms-bmp") != std::string::npos) {
        return "bmp";
    }
    return "unknown";
}

inline std::string MultipartFormDataParser::ExtractNameFromDisposition(
    const std::unordered_map<std::string, std::string>& params) {
    auto it = params.find("name");
    if (it != params.end()) {
        return it->second;
    }
    return "";
}

}  // namespace handler
}  // namespace gate
}  // namespace mould
