#include "channel_topology_config.hpp"

#include <algorithm>
#include <cstdint>
#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sstream>
#include <set>
#include <unordered_set>
#include <utility>

namespace mould::config {
namespace {

std::string Trim(std::string value) {
  const auto begin = std::find_if(value.begin(), value.end(), [](unsigned char c) {
    return !std::isspace(c);
  });
  const auto end = std::find_if(value.rbegin(), value.rend(), [](unsigned char c) {
    return !std::isspace(c);
  }).base();
  if (begin >= end) {
    return "";
  }
  return std::string(begin, end);
}

bool IsAllowedChannelChar(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
      (value >= '0' && value <= '9') || value == '_' || value == '-' || value == '/' ||
      value == '.';
}

bool IsValidChannelName(const std::string& channel) {
  if (channel.empty()) {
    return false;
  }
  for (const char value : channel) {
    if (!IsAllowedChannelChar(value)) {
      return false;
    }
  }
  return true;
}

bool ParseParamToken(
    const std::string& token,
    std::unordered_map<std::string, std::string>* out_params,
    std::string* out_error) {
  const auto equals_pos = token.find('=');
  if (equals_pos == std::string::npos || equals_pos == 0 || equals_pos + 1 >= token.size()) {
    if (out_error != nullptr) {
      *out_error = "invalid parameter token: " + token;
    }
    return false;
  }

  const std::string key = token.substr(0, equals_pos);
  const std::string value = token.substr(equals_pos + 1);
  if (key.empty() || value.empty()) {
    if (out_error != nullptr) {
      *out_error = "empty key or value in parameter token: " + token;
    }
    return false;
  }

  auto [iter, inserted] = out_params->emplace(key, value);
  if (!inserted && iter->second != value) {
    if (out_error != nullptr) {
      *out_error = "conflicting duplicate parameter '" + key + "'";
    }
    return false;
  }
  return true;
}

ChannelPayloadType ParseChannelPayloadType(const std::string& value) {
  if (value.empty() || value == "protobuf" || value == "proto") {
    return ChannelPayloadType::kProtobuf;
  }
  if (value == "image" || value == "image_binary_meta") {
    return ChannelPayloadType::kImage;
  }
  if (value == "binary" || value == "binary_blob" || value == "raw") {
    return ChannelPayloadType::kBinaryBlob;
  }
  return ChannelPayloadType::kUnknown;
}

std::string ParamOrEmpty(
    const std::unordered_map<std::string, std::string>& params,
    const std::string& key) {
  const auto iter = params.find(key);
  if (iter == params.end()) {
    return {};
  }
  return iter->second;
}

void MergeModule(
    const ModuleChannelConfig& module_config,
    ChannelTopologyIndex* topology,
    std::string* out_error,
    bool* ok) {
  auto merge_params = [&](const std::string& channel, const std::unordered_map<std::string, std::string>& params) {
    auto& entry = (*topology)[channel];
    entry.channel = channel;
    for (const auto& [key, value] : params) {
      auto [iter, inserted] = entry.params.emplace(key, value);
      if (!inserted && iter->second != value) {
        *ok = false;
        if (out_error != nullptr) {
          *out_error = "parameter conflict for channel '" + channel + "' key '" + key + "'";
        }
        return;
      }
    }
  };

  for (const auto& output : module_config.output_channels) {
    if (!*ok) {
      return;
    }
    merge_params(output.channel, output.params);
    if (!*ok) {
      return;
    }
    auto& entry = (*topology)[output.channel];
    if (std::find(entry.producers.begin(), entry.producers.end(), module_config.module_name) ==
        entry.producers.end()) {
      entry.producers.push_back(module_config.module_name);
    }
  }

  for (const auto& input : module_config.input_channels) {
    if (!*ok) {
      return;
    }
    merge_params(input.channel, input.params);
    if (!*ok) {
      return;
    }
    auto& entry = (*topology)[input.channel];
    if (std::find(entry.consumers.begin(), entry.consumers.end(), module_config.module_name) ==
        entry.consumers.end()) {
      entry.consumers.push_back(module_config.module_name);
      entry.consumer_count = entry.consumers.size();
    }
  }
}

std::size_t ParsePositiveSizeOrZero(const std::string& value) {
  if (value.empty()) {
    return 0;
  }
  for (const char c : value) {
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      return 0;
    }
  }
  return static_cast<std::size_t>(std::stoull(value));
}

struct JsonValue;
using JsonObject = std::vector<std::pair<std::string, JsonValue>>;

struct JsonValue {
  enum Kind { kNull, kBool, kNumber, kString, kObject, kArray } kind = kNull;
  bool b = false;
  double number = 0.0;
  bool number_is_integral = false;
  std::int64_t integral = 0;
  std::string s;
  JsonObject o;
  std::vector<JsonValue> a;
};

class JsonParseError : public std::runtime_error {
 public:
  explicit JsonParseError(std::string message) : std::runtime_error(std::move(message)) {}
};

class JsonParser {
 public:
  explicit JsonParser(std::string_view input) : input_(input) {}

  JsonValue ParseDocument() {
    SkipWhitespace();
    JsonValue value = ParseValue();
    SkipWhitespace();
    if (position_ < input_.size()) {
      throw JsonParseError("trailing characters after JSON document");
    }
    return value;
  }

 private:
  std::string_view input_;
  std::size_t position_ = 0;

  char Peek() const { return position_ < input_.size() ? input_[position_] : '\0'; }

  void Advance() {
    if (position_ < input_.size()) {
      ++position_;
    }
  }

  void SkipWhitespace() {
    while (position_ < input_.size()) {
      const char c = input_[position_];
      if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
        ++position_;
        continue;
      }
      break;
    }
  }

  void Expect(char c) {
    SkipWhitespace();
    if (Peek() != c) {
      throw JsonParseError(std::string("expected '") + c + "'");
    }
    Advance();
  }

  JsonValue ParseValue() {
    SkipWhitespace();
    const char c = Peek();
    if (c == '{') {
      return ParseObject();
    }
    if (c == '[') {
      return ParseArray();
    }
    if (c == '"') {
      return ParseStringValue();
    }
    if (c == 't' || c == 'f') {
      return ParseBool();
    }
    if (c == 'n') {
      return ParseNull();
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      return ParseNumber();
    }
    throw JsonParseError(
        "invalid JSON value at pos " + std::to_string(position_) + " near '" +
        std::string(1, c == '\0' ? '#' : c) + "'");
  }

  JsonValue ParseObject() {
    Expect('{');
    JsonObject object;
    SkipWhitespace();
    if (Peek() == '}') {
      Advance();
      JsonValue value;
      value.kind = JsonValue::kObject;
      value.o = std::move(object);
      return value;
    }

    for (;;) {
      SkipWhitespace();
      if (Peek() != '"') {
        throw JsonParseError("object key must be string");
      }
      Advance();
      const std::string key = ParseString();
      Expect(':');
      JsonValue value = ParseValue();
      for (const auto& [existing_key, _] : object) {
        if (existing_key == key) {
          throw JsonParseError("duplicate key '" + key + "'");
        }
      }
      object.emplace_back(key, std::move(value));
      SkipWhitespace();
      if (Peek() == '}') {
        Advance();
        break;
      }
      if (Peek() != ',') {
        throw JsonParseError("expected ',' or '}' in object");
      }
      Advance();
    }
    JsonValue value;
    value.kind = JsonValue::kObject;
    value.o = std::move(object);
    return value;
  }

  JsonValue ParseArray() {
    Expect('[');
    std::vector<JsonValue> values;
    SkipWhitespace();
    if (Peek() == ']') {
      Advance();
      JsonValue v;
      v.kind = JsonValue::kArray;
      v.a = std::move(values);
      return v;
    }
    for (;;) {
      values.push_back(ParseValue());
      SkipWhitespace();
      if (Peek() == ']') {
        Advance();
        break;
      }
      if (Peek() != ',') {
        throw JsonParseError("expected ',' or ']' in array");
      }
      Advance();
    }
    JsonValue v;
    v.kind = JsonValue::kArray;
    v.a = std::move(values);
    return v;
  }

  JsonValue ParseStringValue() {
    Expect('"');
    JsonValue value;
    value.kind = JsonValue::kString;
    value.s = ParseString();
    return value;
  }

  std::string ParseString() {
    std::string out;
    while (position_ < input_.size()) {
      const char c = input_[position_++];
      if (c == '"') {
        return out;
      }
      if (c == '\\') {
        if (position_ >= input_.size()) {
          throw JsonParseError("unterminated string escape");
        }
        const char e = input_[position_++];
        switch (e) {
          case '"':
          case '\\':
          case '/':
            out.push_back(e);
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          default:
            throw JsonParseError("unsupported string escape");
        }
        continue;
      }
      out.push_back(c);
    }
    throw JsonParseError("unterminated string");
  }

  JsonValue ParseBool() {
    JsonValue value;
    value.kind = JsonValue::kBool;
    if (input_.substr(position_, 4) == "true") {
      position_ += 4;
      value.b = true;
      return value;
    }
    if (input_.substr(position_, 5) == "false") {
      position_ += 5;
      value.b = false;
      return value;
    }
    throw JsonParseError("invalid boolean literal");
  }

  JsonValue ParseNull() {
    if (input_.substr(position_, 4) != "null") {
      throw JsonParseError("invalid null literal");
    }
    position_ += 4;
    JsonValue value;
    value.kind = JsonValue::kNull;
    return value;
  }

  JsonValue ParseNumber() {
    const std::size_t start = position_;
    if (Peek() == '-') {
      Advance();
    }
    while (Peek() >= '0' && Peek() <= '9') {
      Advance();
    }
    if (Peek() == '.') {
      Advance();
      while (Peek() >= '0' && Peek() <= '9') {
        Advance();
      }
    }
    if (Peek() == 'e' || Peek() == 'E') {
      Advance();
      if (Peek() == '+' || Peek() == '-') {
        Advance();
      }
      while (Peek() >= '0' && Peek() <= '9') {
        Advance();
      }
    }
    const std::string token(input_.substr(start, position_ - start));
    JsonValue value;
    value.kind = JsonValue::kNumber;
    if (token.find('.') != std::string::npos || token.find('e') != std::string::npos ||
        token.find('E') != std::string::npos) {
      value.number = std::stod(token);
      value.number_is_integral = false;
    } else {
      value.integral = std::stoll(token);
      value.number = static_cast<double>(value.integral);
      value.number_is_integral = true;
    }
    return value;
  }
};

const JsonValue* FindJsonKey(const JsonObject& object, const std::string& key) {
  for (const auto& [k, v] : object) {
    if (k == key) {
      return &v;
    }
  }
  return nullptr;
}

std::string JsonScalarToString(const JsonValue& value) {
  if (value.kind == JsonValue::kString) {
    return value.s;
  }
  if (value.kind == JsonValue::kBool) {
    return value.b ? "true" : "false";
  }
  if (value.kind == JsonValue::kNumber) {
    if (value.number_is_integral) {
      return std::to_string(value.integral);
    }
    std::ostringstream oss;
    oss << value.number;
    return oss.str();
  }
  return {};
}

bool ParseModuleChannelConfigJsonFile(
    const std::string& module_name,
    const std::string& config_path,
    ModuleChannelConfig* out_config,
    std::string* out_error) {
  if (out_config == nullptr) {
    if (out_error != nullptr) {
      *out_error = "output config pointer is null";
    }
    return false;
  }

  std::ifstream input(config_path, std::ios::binary);
  if (!input.is_open()) {
    if (out_error != nullptr) {
      *out_error = "failed to open config file: " + config_path;
    }
    return false;
  }

  std::ostringstream buffer;
  buffer << input.rdbuf();
  const std::string json_text = buffer.str();

  JsonValue root;
  try {
    JsonParser parser(json_text);
    root = parser.ParseDocument();
  } catch (const JsonParseError& e) {
    if (out_error != nullptr) {
      *out_error = "invalid JSON in " + config_path + ": " + e.what();
    }
    return false;
  }

  if (root.kind != JsonValue::kObject) {
    if (out_error != nullptr) {
      *out_error = "root must be JSON object in " + config_path;
    }
    return false;
  }

  const JsonValue* input_node = FindJsonKey(root.o, "input_channel");
  const JsonValue* output_node = FindJsonKey(root.o, "output_channel");
  if (input_node == nullptr || output_node == nullptr) {
    if (out_error != nullptr) {
      *out_error = "JSON config must contain input_channel and output_channel: " + config_path;
    }
    return false;
  }
  if (input_node->kind != JsonValue::kObject || output_node->kind != JsonValue::kObject) {
    if (out_error != nullptr) {
      *out_error = "input_channel/output_channel must be objects: " + config_path;
    }
    return false;
  }

  ModuleChannelConfig parsed;
  parsed.module_name = module_name;
  std::unordered_set<std::string> seen_output_channels;

  auto parse_side = [&](const JsonValue& side, bool is_output) -> bool {
    for (const auto& [channel_name, params_node] : side.o) {
      if (!IsValidChannelName(channel_name)) {
        if (out_error != nullptr) {
          *out_error = "invalid channel name '" + channel_name + "' in " + config_path;
        }
        return false;
      }
      if (params_node.kind != JsonValue::kObject) {
        if (out_error != nullptr) {
          *out_error = "channel params must be object for channel '" + channel_name + "' in " + config_path;
        }
        return false;
      }
      ChannelEndpointConfig endpoint;
      endpoint.channel = channel_name;
      for (const auto& [param_key, param_value] : params_node.o) {
        const std::string scalar = JsonScalarToString(param_value);
        if (scalar.empty() && param_value.kind != JsonValue::kString) {
          if (out_error != nullptr) {
            *out_error = "channel param must be scalar for key '" + param_key + "' in " + config_path;
          }
          return false;
        }
        endpoint.params[param_key] = scalar;
      }
      if (is_output) {
        if (!seen_output_channels.insert(channel_name).second) {
          if (out_error != nullptr) {
            *out_error = "duplicate output channel in one module: " + channel_name;
          }
          return false;
        }
        parsed.output_channels.push_back(std::move(endpoint));
      } else {
        parsed.input_channels.push_back(std::move(endpoint));
      }
    }
    return true;
  };

  if (!parse_side(*input_node, false) || !parse_side(*output_node, true)) {
    return false;
  }

  *out_config = std::move(parsed);
  return true;
}

}  // namespace

bool ParseModuleChannelConfigFile(
    const std::string& module_name,
    const std::string& config_path,
    ModuleChannelConfig* out_config,
    std::string* out_error) {
  if (out_config == nullptr) {
    if (out_error != nullptr) {
      *out_error = "output config pointer is null";
    }
    return false;
  }
  std::ifstream input(config_path, std::ios::binary);
  if (!input.is_open()) {
    if (out_error != nullptr) {
      *out_error = "failed to open config file: " + config_path;
    }
    return false;
  }

  std::ostringstream buffer;
  buffer << input.rdbuf();
  const std::string file_content = buffer.str();
  const auto first_non_ws = file_content.find_first_not_of(" \t\r\n");
  if (first_non_ws != std::string::npos && file_content[first_non_ws] == '{') {
    return ParseModuleChannelConfigJsonFile(module_name, config_path, out_config, out_error);
  }

  ModuleChannelConfig parsed;
  parsed.module_name = module_name;

  std::unordered_set<std::string> seen_output_channels;
  std::string line;
  std::size_t line_no = 0;
  std::istringstream line_stream(file_content);
  while (std::getline(line_stream, line)) {
    ++line_no;
    std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }

    std::istringstream iss(trimmed);
    std::string role;
    std::string channel;
    iss >> role >> channel;
    if (role.empty() || channel.empty()) {
      if (out_error != nullptr) {
        *out_error = "line " + std::to_string(line_no) + ": expected '<input|output> <channel>'";
      }
      return false;
    }
    if (!role.empty() && role.back() == ':') {
      role.pop_back();
    }
    if (role != "input" && role != "output") {
      if (out_error != nullptr) {
        *out_error = "line " + std::to_string(line_no) + ": role must be input or output";
      }
      return false;
    }
    if (!IsValidChannelName(channel)) {
      if (out_error != nullptr) {
        *out_error = "line " + std::to_string(line_no) + ": invalid channel name '" + channel + "'";
      }
      return false;
    }

    ChannelEndpointConfig endpoint;
    endpoint.channel = channel;
    std::string token;
    while (iss >> token) {
      if (!ParseParamToken(token, &endpoint.params, out_error)) {
        if (out_error != nullptr) {
          *out_error = "line " + std::to_string(line_no) + ": " + *out_error;
        }
        return false;
      }
    }

    if (role == "output") {
      const auto [iter, inserted] = seen_output_channels.insert(channel);
      (void)iter;
      if (!inserted) {
        if (out_error != nullptr) {
          *out_error = "line " + std::to_string(line_no) +
              ": duplicate output channel in one module: " + channel;
        }
        return false;
      }
      parsed.output_channels.push_back(std::move(endpoint));
    } else {
      parsed.input_channels.push_back(std::move(endpoint));
    }
  }

  *out_config = std::move(parsed);
  return true;
}

bool BuildChannelTopologyIndex(
    const std::vector<ModuleChannelConfig>& module_configs,
    ChannelTopologyIndex* out_topology,
    std::string* out_error) {
  if (out_topology == nullptr) {
    if (out_error != nullptr) {
      *out_error = "output topology pointer is null";
    }
    return false;
  }

  ChannelTopologyIndex topology;
  bool ok = true;
  for (const auto& module_config : module_configs) {
    MergeModule(module_config, &topology, out_error, &ok);
    if (!ok) {
      return false;
    }
  }

  for (auto& [channel, entry] : topology) {
    std::sort(entry.producers.begin(), entry.producers.end());
    std::sort(entry.consumers.begin(), entry.consumers.end());
    entry.consumer_count = entry.consumers.size();
    if (entry.producers.size() > 1) {
      if (out_error != nullptr) {
        *out_error = "channel '" + channel + "' has multiple producers";
      }
      return false;
    }
    const auto delivery_iter = entry.params.find("delivery_mode");
    if (delivery_iter != entry.params.end() && !delivery_iter->second.empty() &&
        !IsValidShmBusDeliveryModeParamValue(delivery_iter->second)) {
      if (out_error != nullptr) {
        *out_error = "channel '" + channel + "': invalid delivery_mode '" + delivery_iter->second +
            "' (use broadcast or compete)";
      }
      return false;
    }
    if (!ValidateChannelSchemaGovernance(entry, out_error)) {
      return false;
    }
  }

  *out_topology = std::move(topology);
  return true;
}

bool BuildChannelTopologyIndexFromFiles(
    const std::vector<std::pair<std::string, std::string>>& module_config_files,
    ChannelTopologyIndex* out_topology,
    std::string* out_error) {
  std::vector<ModuleChannelConfig> configs;
  configs.reserve(module_config_files.size());
  for (const auto& [module_name, config_path] : module_config_files) {
    ModuleChannelConfig config;
    if (!ParseModuleChannelConfigJsonFile(module_name, config_path, &config, out_error)) {
      return false;
    }
    configs.push_back(std::move(config));
  }
  return BuildChannelTopologyIndex(configs, out_topology, out_error);
}

const char* ChannelPayloadTypeName(ChannelPayloadType type) {
  switch (type) {
    case ChannelPayloadType::kProtobuf:
      return "protobuf";
    case ChannelPayloadType::kImage:
      return "image";
    case ChannelPayloadType::kBinaryBlob:
      return "binary";
    case ChannelPayloadType::kUnknown:
      return "unknown";
  }
  return "unknown";
}

ChannelPayloadType ResolveChannelPayloadType(const ChannelTopologyEntry& entry) {
  return ParseChannelPayloadType(ParamOrEmpty(entry.params, "payload_type"));
}

bool ValidateChannelSchemaGovernance(
    const ChannelTopologyEntry& entry,
    std::string* out_error) {
  const ChannelPayloadType payload_type = ResolveChannelPayloadType(entry);
  if (payload_type == ChannelPayloadType::kUnknown) {
    if (out_error != nullptr) {
      *out_error = "unknown payload_type for channel '" + entry.channel + "': " +
          ParamOrEmpty(entry.params, "payload_type");
    }
    return false;
  }
  if (payload_type != ChannelPayloadType::kProtobuf) {
    return true;
  }
  const std::string proto_message = ParamOrEmpty(entry.params, "proto_message");
  if (!proto_message.empty() && proto_message != entry.channel) {
    if (out_error != nullptr) {
      *out_error = "channel schema mismatch: channel '" + entry.channel +
          "' must equal protobuf message name '" + proto_message + "'";
    }
    return false;
  }
  return true;
}

bool ValidateProtoReservedFieldNumbers(
    const std::string& proto_text,
    std::string* out_error) {
  std::set<std::int64_t> reserved_numbers;
  std::istringstream lines(proto_text);
  std::string line;
  while (std::getline(lines, line)) {
    const std::string trimmed = Trim(line);
    if (trimmed.rfind("reserved ", 0) == 0) {
      std::istringstream iss(trimmed.substr(std::string("reserved ").size()));
      std::string token;
      while (std::getline(iss, token, ',')) {
        token = Trim(token);
        if (!token.empty() && token.back() == ';') {
          token.pop_back();
        }
        if (!token.empty() && std::all_of(token.begin(), token.end(), [](unsigned char c) {
              return std::isdigit(c);
            })) {
          reserved_numbers.insert(std::stoll(token));
        }
      }
      continue;
    }
    const auto equals_pos = trimmed.find('=');
    if (equals_pos == std::string::npos) {
      continue;
    }
    std::size_t number_begin = equals_pos + 1U;
    while (number_begin < trimmed.size() &&
           std::isspace(static_cast<unsigned char>(trimmed[number_begin]))) {
      ++number_begin;
    }
    std::size_t number_end = number_begin;
    while (number_end < trimmed.size() &&
           std::isdigit(static_cast<unsigned char>(trimmed[number_end]))) {
      ++number_end;
    }
    if (number_begin == number_end) {
      continue;
    }
    const std::int64_t field_number = std::stoll(trimmed.substr(number_begin, number_end - number_begin));
    if (reserved_numbers.find(field_number) != reserved_numbers.end()) {
      if (out_error != nullptr) {
        *out_error = "protobuf field number " + std::to_string(field_number) +
            " reuses a reserved number";
      }
      return false;
    }
  }
  return true;
}

namespace {

std::string ToLowerAscii(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (char c : value) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

}  // namespace

bool IsValidShmBusDeliveryModeParamValue(const std::string& value) {
  if (value.empty()) {
    return true;
  }
  const std::string lower = ToLowerAscii(value);
  return lower == "broadcast" || lower == "compete";
}

ShmBusDeliveryMode ResolveShmBusDeliveryModeForChannel(const ChannelTopologyEntry* entry) {
  if (entry == nullptr) {
    return ShmBusDeliveryMode::kBroadcast;
  }
  const auto iter = entry->params.find("delivery_mode");
  if (iter == entry->params.end() || iter->second.empty()) {
    return ShmBusDeliveryMode::kBroadcast;
  }
  const std::string lower = ToLowerAscii(iter->second);
  if (lower == "compete") {
    return ShmBusDeliveryMode::kCompete;
  }
  return ShmBusDeliveryMode::kBroadcast;
}

std::uint32_t ResolveShmRingConsumerCapacity(
    const ChannelTopologyEntry* entry,
    const std::uint32_t default_consumer_slots_per_channel) {
  const std::uint32_t floor_default = std::max<std::uint32_t>(1U, default_consumer_slots_per_channel);
  if (entry == nullptr) {
    return floor_default;
  }
  std::uint32_t resolved = floor_default;
  const auto configured_iter = entry->params.find("shm_consumer_slots");
  if (configured_iter != entry->params.end()) {
    const std::size_t configured = ParsePositiveSizeOrZero(configured_iter->second);
    if (configured > 0 && configured <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
      resolved = static_cast<std::uint32_t>(configured);
    }
  }
  if (entry->consumer_count == 0) {
    return resolved;
  }
  const auto topology_consumer_count = static_cast<std::uint32_t>(entry->consumer_count);
  return std::max(resolved, topology_consumer_count);
}

std::size_t ResolveSlotPayloadBytesForChannel(
    const ChannelTopologyEntry* entry,
    const std::size_t default_slot_payload_bytes) {
  std::size_t resolved = std::max<std::size_t>(1, default_slot_payload_bytes);
  if (entry == nullptr) {
    return resolved;
  }
  const auto slot_payload_iter = entry->params.find("slot_payload_bytes");
  if (slot_payload_iter != entry->params.end()) {
    const std::size_t configured = ParsePositiveSizeOrZero(slot_payload_iter->second);
    if (configured > 0) {
      return configured;
    }
  }
  return resolved;
}

std::uint32_t ResolveShmSlotCountForChannel(
    const ChannelTopologyEntry* entry,
    const std::uint32_t default_shm_slot_count) {
  const std::uint32_t resolved = std::max<std::uint32_t>(1U, default_shm_slot_count);
  if (entry == nullptr) {
    return resolved;
  }
  const auto slot_count_iter = entry->params.find("shm_slot_count");
  if (slot_count_iter == entry->params.end()) {
    return resolved;
  }
  const std::size_t configured = ParsePositiveSizeOrZero(slot_count_iter->second);
  if (configured == 0 || configured > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return resolved;
  }
  return static_cast<std::uint32_t>(configured);
}

std::string CanonicalShmChannelKey(const ChannelTopologyEntry& entry) {
  if (!entry.producers.empty()) {
    return entry.producers.front() + "__" + entry.channel;
  }
  if (!entry.consumers.empty()) {
    return entry.consumers.front() + "__" + entry.channel;
  }
  return entry.channel;
}

}  // namespace mould::config
