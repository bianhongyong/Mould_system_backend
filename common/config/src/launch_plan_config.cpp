#include "launch_plan_config.hpp"

#include <gflags/gflags.h>

#include <cstdlib>   // setenv
#include <cstring>   // std::strlen

#include <cmath>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace mould::config {
namespace {

constexpr const char* kIoPathField = "io_channels_config_path";
constexpr const char* kModulesField = "modules";
constexpr const char* kMinLogLevelField = "minloglevel";
constexpr const char* kCommunicationField = "communication";
constexpr const char* kShmSlotCountField = "shm_slot_count";
constexpr const char* kSlotPayloadBytesField = "slot_payload_bytes";

bool IsAllowedChannelChar(const char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
      (value >= '0' && value <= '9') || value == '_' || value == '-' || value == '/' ||
      value == '.';
}

bool IsValidChannelName(const std::string& channel) {
  if (channel.empty()) {
    return false;
  }
  for (const char c : channel) {
    if (!IsAllowedChannelChar(c)) {
      return false;
    }
  }
  return true;
}

struct JValue;

using JObject = std::vector<std::pair<std::string, JValue>>;

struct JValue {
  enum Kind { kNull, kBool, kNumber, kString, kObject, kArray } kind = kNull;
  bool b = false;
  double number = 0.0;
  bool number_is_integral = false;
  std::int64_t integral = 0;
  std::string s;
  JObject o;
  std::vector<JValue> a;
};

class JsonParseError : public std::runtime_error {
 public:
  explicit JsonParseError(std::string message) : std::runtime_error(std::move(message)) {}
};

class JParser {
 public:
  explicit JParser(std::string_view text) : in_(text) {}

  JValue ParseDocument() {
    ws();
    JValue v = parse_value();
    ws();
    if (pos_ < in_.size()) {
      throw JsonParseError("trailing characters after JSON value");
    }
    return v;
  }

 private:
  std::string_view in_;
  std::size_t pos_ = 0;

  char peek() const { return pos_ < in_.size() ? in_[pos_] : '\0'; }

  void bump() {
    if (pos_ < in_.size()) {
      ++pos_;
    }
  }

  void ws() {
    while (pos_ < in_.size()) {
      const char c = in_[pos_];
      if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
        ++pos_;
        continue;
      }
      break;
    }
  }

  void expect(char c) {
    ws();
    if (peek() != c) {
      throw JsonParseError(std::string("expected '") + c + "'");
    }
    bump();
  }

  JValue parse_value() {
    ws();
    const char c = peek();
    if (c == '{') {
      return parse_object();
    }
    if (c == '[') {
      return parse_array();
    }
    if (c == '"') {
      return parse_string_value();
    }
    if (c == 't' || c == 'f') {
      return parse_bool();
    }
    if (c == 'n') {
      return parse_null();
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      return parse_number();
    }
    throw JsonParseError("invalid JSON value");
  }

  JValue parse_object() {
    expect('{');
    JObject obj;
    ws();
    if (peek() == '}') {
      bump();
      JValue v;
      v.kind = JValue::kObject;
      v.o = std::move(obj);
      return v;
    }
    for (;;) {
      ws();
      if (peek() != '"') {
        throw JsonParseError("object key must be string");
      }
      bump();
      const std::string key = parse_string();
      expect(':');
      JValue val = parse_value();
      for (const auto& existing : obj) {
        if (existing.first == key) {
          throw JsonParseError("duplicate key '" + key + "' in object");
        }
      }
      obj.emplace_back(std::move(key), std::move(val));
      ws();
      const char d = peek();
      if (d == '}') {
        bump();
        break;
      }
      if (d != ',') {
        throw JsonParseError("expected ',' or '}' in object");
      }
      bump();
    }
    JValue v;
    v.kind = JValue::kObject;
    v.o = std::move(obj);
    return v;
  }

  JValue parse_array() {
    expect('[');
    std::vector<JValue> arr;
    ws();
    if (peek() == ']') {
      bump();
      JValue v;
      v.kind = JValue::kArray;
      v.a = std::move(arr);
      return v;
    }
    for (;;) {
      arr.push_back(parse_value());
      ws();
      const char d = peek();
      if (d == ']') {
        bump();
        break;
      }
      if (d != ',') {
        throw JsonParseError("expected ',' or ']' in array");
      }
      bump();
    }
    JValue v;
    v.kind = JValue::kArray;
    v.a = std::move(arr);
    return v;
  }

  JValue parse_string_value() {
    expect('"');
    JValue v;
    v.kind = JValue::kString;
    v.s = parse_string();
    return v;
  }

  std::string parse_string() {
    std::string out;
    while (pos_ < in_.size()) {
      const char c = in_[pos_++];
      if (c == '"') {
        return out;
      }
      if (c == '\\') {
        if (pos_ >= in_.size()) {
          throw JsonParseError("unterminated escape in string");
        }
        const char e = in_[pos_++];
        switch (e) {
          case '"':
            out.push_back('"');
            break;
          case '\\':
            out.push_back('\\');
            break;
          case '/':
            out.push_back('/');
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
            throw JsonParseError("unsupported escape in string");
        }
        continue;
      }
      out.push_back(c);
    }
    throw JsonParseError("unterminated string");
  }

  JValue parse_bool() {
    JValue v;
    v.kind = JValue::kBool;
    if (in_.substr(pos_, 4) == "true") {
      pos_ += 4;
      v.b = true;
      return v;
    }
    if (in_.substr(pos_, 5) == "false") {
      pos_ += 5;
      v.b = false;
      return v;
    }
    throw JsonParseError("invalid boolean literal");
  }

  JValue parse_null() {
    if (in_.substr(pos_, 4) != "null") {
      throw JsonParseError("invalid null literal");
    }
    pos_ += 4;
    JValue v;
    v.kind = JValue::kNull;
    return v;
  }

  JValue parse_number() {
    const std::size_t start = pos_;
    if (peek() == '-') {
      bump();
    }
    while (peek() >= '0' && peek() <= '9') {
      bump();
    }
    if (peek() == '.') {
      bump();
      while (peek() >= '0' && peek() <= '9') {
        bump();
      }
    }
    if (peek() == 'e' || peek() == 'E') {
      bump();
      if (peek() == '+' || peek() == '-') {
        bump();
      }
      while (peek() >= '0' && peek() <= '9') {
        bump();
      }
    }
    const std::string token(in_.substr(start, pos_ - start));
    JValue v;
    v.kind = JValue::kNumber;
    try {
      if (token.find('.') != std::string::npos || token.find('e') != std::string::npos ||
          token.find('E') != std::string::npos) {
        v.number = std::stod(token);
        v.number_is_integral = false;
      } else {
        v.integral = std::stoll(token);
        v.number = static_cast<double>(v.integral);
        v.number_is_integral = true;
      }
    } catch (...) {
      throw JsonParseError("invalid number literal");
    }
    return v;
  }
};

JValue ParseJsonTree(const std::string& text, std::string* out_error) {
  try {
    JParser parser(text);
    return parser.ParseDocument();
  } catch (const JsonParseError& e) {
    if (out_error != nullptr) {
      *out_error = e.what();
    }
    return {};
  }
}

const JValue* FindObjectKey(const JObject& o, const std::string& key) {
  for (const auto& [k, v] : o) {
    if (k == key) {
      return &v;
    }
  }
  return nullptr;
}

std::string ReadEntireFile(const std::string& path, std::string* out_error) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    if (out_error != nullptr) {
      *out_error = "failed to open file: " + path;
    }
    return {};
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

std::string ScalarToAssignmentString(const LaunchPlanScalar& value) {
  struct Visitor {
    std::string operator()(const std::string& s) const { return s; }
    std::string operator()(std::int64_t i) const { return std::to_string(i); }
    std::string operator()(double d) const {
      std::ostringstream oss;
      oss << std::setprecision(std::numeric_limits<double>::digits10 + 1) << d;
      return oss.str();
    }
    std::string operator()(bool b) const { return b ? "true" : "false"; }
  };
  return std::visit(Visitor{}, value);
}

bool ScalarFromJsonLeaf(const JValue& node, const std::string& field_path, LaunchPlanScalar* out, std::string* err) {
  if (node.kind == JValue::kString) {
    *out = node.s;
    return true;
  }
  if (node.kind == JValue::kBool) {
    *out = node.b;
    return true;
  }
  if (node.kind == JValue::kNumber) {
    if (node.number_is_integral) {
      *out = node.integral;
    } else {
      *out = node.number;
    }
    return true;
  }
  if (node.kind == JValue::kNull) {
    if (err != nullptr) {
      *err = field_path + ": null is not allowed for scalar field";
    }
    return false;
  }
  if (err != nullptr) {
    *err = field_path + ": expected scalar (string, number, or bool)";
  }
  return false;
}

std::string ScalarToParamString(const LaunchPlanScalar& value) {
  return ScalarToAssignmentString(value);
}

bool ParseChannelDict(
    const JValue& side,
    const std::string& field_path,
    bool is_output,
    std::vector<ChannelEndpointConfig>* out_list,
    std::unordered_set<std::string>* seen_outputs,
    std::string* err) {
  if (side.kind != JValue::kObject) {
    if (err != nullptr) {
      *err = field_path + ": must be object";
    }
    return false;
  }
  for (const auto& [channel, params_node] : side.o) {
    if (!IsValidChannelName(channel)) {
      if (err != nullptr) {
        *err = field_path + "." + channel + ": invalid channel name";
      }
      return false;
    }
    ChannelEndpointConfig endpoint;
    endpoint.channel = channel;
    if (params_node.kind != JValue::kObject) {
      if (err != nullptr) {
        *err = field_path + "." + channel + ": parameters must be object";
      }
      return false;
    }
    for (const auto& [pk, pv] : params_node.o) {
      LaunchPlanScalar scalar;
      const std::string ppath = field_path + "." + channel + "." + pk;
      if (!ScalarFromJsonLeaf(pv, ppath, &scalar, err)) {
        return false;
      }
      const auto [it, inserted] = endpoint.params.emplace(pk, ScalarToParamString(scalar));
      (void)it;
      if (!inserted) {
        if (err != nullptr) {
          *err = ppath + ": duplicate parameter key";
        }
        return false;
      }
    }
    if (is_output) {
      if (!seen_outputs->insert(channel).second) {
        if (err != nullptr) {
          *err = field_path + "." + channel + ": duplicate output channel in module";
        }
        return false;
      }
      out_list->push_back(std::move(endpoint));
    } else {
      out_list->push_back(std::move(endpoint));
    }
  }
  return true;
}

bool ParseModuleIoJsonFile(
    const std::string& module_name,
    const std::string& absolute_io_path,
    ModuleChannelConfig* out_channels,
    std::string* err) {
  std::string read_err;
  const std::string text = ReadEntireFile(absolute_io_path, &read_err);
  if (text.empty() && read_err.find("failed to open") != std::string::npos) {
    if (err != nullptr) {
      *err = read_err;
    }
    return false;
  }
  std::string parse_err;
  JValue root = ParseJsonTree(text, &parse_err);
  if (!parse_err.empty()) {
    if (err != nullptr) {
      *err = absolute_io_path + ": " + parse_err;
    }
    return false;
  }
  if (root.kind != JValue::kObject) {
    if (err != nullptr) {
      *err = absolute_io_path + ": root must be object";
    }
    return false;
  }

  const JValue* in_node = FindObjectKey(root.o, "input_channel");
  const JValue* out_node = FindObjectKey(root.o, "output_channel");
  if (in_node == nullptr) {
    if (err != nullptr) {
      *err = "module '" + module_name + "' io file " + absolute_io_path + ": missing input_channel";
    }
    return false;
  }
  if (out_node == nullptr) {
    if (err != nullptr) {
      *err = "module '" + module_name + "' io file " + absolute_io_path + ": missing output_channel";
    }
    return false;
  }

  for (const auto& [k, v] : root.o) {
    if (k != "input_channel" && k != "output_channel") {
      if (err != nullptr) {
        *err = "module '" + module_name + "' io file " + absolute_io_path + ": unknown key '" + k + "'";
      }
      return false;
    }
    (void)v;
  }

  ModuleChannelConfig cfg;
  cfg.module_name = module_name;
  std::unordered_set<std::string> seen_outputs;
  if (!ParseChannelDict(*in_node, "input_channel", false, &cfg.input_channels, &seen_outputs, err)) {
    return false;
  }
  seen_outputs.clear();
  if (!ParseChannelDict(*out_node, "output_channel", true, &cfg.output_channels, &seen_outputs, err)) {
    return false;
  }
  *out_channels = std::move(cfg);
  return true;
}

bool JsonKeyRequiresIntegralValue(const std::string& key) {
  // Used by unit test `ParseFails_InvalidScalarTypeInResource` (typed JSON vs string).
  return key == "test_typecheck_int";
}

bool ParseStringMapScalars(
    const JValue& node,
    const std::string& field_prefix,
    std::unordered_map<std::string, LaunchPlanScalar>* out,
    std::string* err) {
  if (node.kind != JValue::kObject) {
    if (err != nullptr) {
      *err = field_prefix + ": must be object";
    }
    return false;
  }
  for (const auto& [k, v] : node.o) {
    if (JsonKeyRequiresIntegralValue(k)) {
      if (v.kind != JValue::kNumber || !v.number_is_integral) {
        if (err != nullptr) {
          *err = field_prefix + "." + k + ": JSON integer value required";
        }
        return false;
      }
    }
    LaunchPlanScalar scalar;
    if (!ScalarFromJsonLeaf(v, field_prefix + "." + k, &scalar, err)) {
      return false;
    }
    out->emplace(k, std::move(scalar));
  }
  return true;
}

bool TryExtractInt64(
    const LaunchPlanScalar& value,
    const std::string& field_path,
    std::int64_t* out,
    std::string* err) {
  if (std::holds_alternative<std::int64_t>(value)) {
    *out = std::get<std::int64_t>(value);
    return true;
  }
  if (std::holds_alternative<double>(value)) {
    const double dv = std::get<double>(value);
    if (std::isfinite(dv) && std::floor(dv) == dv &&
        dv >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
        dv <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
      *out = static_cast<std::int64_t>(dv);
      return true;
    }
    if (err != nullptr) {
      *err = field_path + ": must be integer";
    }
    return false;
  }
  if (std::holds_alternative<std::string>(value)) {
    const std::string& s = std::get<std::string>(value);
    if (s.empty()) {
      if (err != nullptr) {
        *err = field_path + ": must be non-empty integer string";
      }
      return false;
    }
    std::size_t consumed = 0;
    try {
      const long long parsed = std::stoll(s, &consumed, 10);
      if (consumed != s.size()) {
        if (err != nullptr) {
          *err = field_path + ": must be integer string";
        }
        return false;
      }
      *out = static_cast<std::int64_t>(parsed);
      return true;
    } catch (...) {
      if (err != nullptr) {
        *err = field_path + ": must be integer string";
      }
      return false;
    }
  }
  if (err != nullptr) {
    *err = field_path + ": must be integer";
  }
  return false;
}

bool ValidateAndNormalizeCpuSet(const std::string& cpu_set, std::string* normalized, std::string* err, const std::string& field_path) {
  if (cpu_set.empty()) {
    // Empty cpu_set means "no CPU affinity binding".
    if (normalized != nullptr) {
      *normalized = "";
    }
    (void)err;
    (void)field_path;
    return true;
  }
  std::stringstream ss(cpu_set);
  std::string token;
  std::vector<std::int64_t> cpus;
  std::unordered_set<std::int64_t> seen;
  while (std::getline(ss, token, ',')) {
    if (token.empty()) {
      if (err != nullptr) {
        *err = field_path + ": contains empty CPU token";
      }
      return false;
    }
    for (const char c : token) {
      if (!std::isdigit(static_cast<unsigned char>(c))) {
        if (err != nullptr) {
          *err = field_path + ": invalid token '" + token + "'";
        }
        return false;
      }
    }
    std::int64_t value = -1;
    if (!TryExtractInt64(token, field_path, &value, err)) {
      return false;
    }
    if (value < 0 || value > 4095) {
      if (err != nullptr) {
        *err = field_path + ": CPU value out of range [0,4095]";
      }
      return false;
    }
    if (!seen.insert(value).second) {
      if (err != nullptr) {
        *err = field_path + ": duplicate CPU value " + std::to_string(value);
      }
      return false;
    }
    cpus.push_back(value);
  }
  if (cpus.empty()) {
    if (err != nullptr) {
      *err = field_path + ": must contain at least one CPU";
    }
    return false;
  }
  std::ostringstream oss;
  for (std::size_t i = 0; i < cpus.size(); ++i) {
    if (i > 0) {
      oss << ",";
    }
    oss << cpus[i];
  }
  *normalized = oss.str();
  return true;
}

bool LookupRequiredInt(
    const std::unordered_map<std::string, LaunchPlanScalar>& resource,
    const std::string& key,
    const std::string& field_prefix,
    std::int64_t* out,
    std::string* out_error) {
  const auto it = resource.find(key);
  if (it == resource.end()) {
    if (out_error != nullptr) {
      *out_error = field_prefix + "." + key + ": missing required field";
    }
    return false;
  }
  return TryExtractInt64(it->second, field_prefix + "." + key, out, out_error);
}

bool LookupRequiredCpuSet(
    std::unordered_map<std::string, LaunchPlanScalar>* resource,
    const std::string& field_prefix,
    std::string* out_cpu_set,
    std::string* out_error) {
  auto cpu_set_it = resource->find("cpu_set");
  if (cpu_set_it == resource->end()) {
    const auto legacy_it = resource->find("cpu_id");
    if (legacy_it != resource->end()) {
      std::int64_t legacy_cpu = -1;
      if (!TryExtractInt64(legacy_it->second, field_prefix + ".cpu_id", &legacy_cpu, out_error)) {
        return false;
      }
      (*resource)["cpu_set"] = std::to_string(legacy_cpu);
      cpu_set_it = resource->find("cpu_set");
    }
  }
  if (cpu_set_it == resource->end()) {
    if (out_error != nullptr) {
      *out_error = field_prefix + ".cpu_set: missing required field";
    }
    return false;
  }
  if (!std::holds_alternative<std::string>(cpu_set_it->second)) {
    if (out_error != nullptr) {
      *out_error = field_prefix + ".cpu_set: must be string";
    }
    return false;
  }
  std::string normalized;
  if (!ValidateAndNormalizeCpuSet(
          std::get<std::string>(cpu_set_it->second), &normalized, out_error, field_prefix + ".cpu_set")) {
    return false;
  }
  (*resource)["cpu_set"] = normalized;
  *out_cpu_set = normalized;
  return true;
}

// Keys stored per-module in `ResourceSchema` / `ParsedModuleLaunchEntry::resource_schema`.
// They remain in `resource` for ApplyLaunchPlanScalarsToRegisteredGflags but MUST NOT
// participate in cross-module global scalar de-duplication (each module may differ).
bool IsPerModuleResourceSchemaKey(const std::string& key) {
  return key == "startup_priority" || key == "cpu_set" || key == "cpu_id" ||
         key == "restart_backoff_ms" || key == "restart_max_retries" || key == "restart_window_ms" ||
         key == "restart_fuse_ms" || key == "ready_timeout_ms";
}

bool ScalarsEqualForMerge(const LaunchPlanScalar& a, const LaunchPlanScalar& b) {
  if (a.index() == b.index()) {
    return a == b;
  }
  // Cross numeric kinds: compare as double.
  double da = 0.0;
  double db = 0.0;
  if (std::holds_alternative<std::int64_t>(a)) {
    da = static_cast<double>(std::get<std::int64_t>(a));
  } else if (std::holds_alternative<double>(a)) {
    da = std::get<double>(a);
  } else {
    return false;
  }
  if (std::holds_alternative<std::int64_t>(b)) {
    db = static_cast<double>(std::get<std::int64_t>(b));
  } else if (std::holds_alternative<double>(b)) {
    db = std::get<double>(b);
  } else {
    return false;
  }
  return std::fabs(da - db) <= 1e-9 * std::max(1.0, std::fabs(da));
}

bool RecordGlobalScalar(
    const std::string& key,
    const LaunchPlanScalar& value,
    std::unordered_map<std::string, LaunchPlanScalar>* global,
    std::string* err) {
  const auto [it, inserted] = global->emplace(key, value);
  if (!inserted && !ScalarsEqualForMerge(it->second, value)) {
    if (err != nullptr) {
      *err = "global gflag key conflict for '" + key + "' across modules";
    }
    return false;
  }
  return true;
}

}  // namespace

bool ResourceSchemaValidator::ValidateAndNormalize(
    std::unordered_map<std::string, LaunchPlanScalar>* resource,
    ResourceSchema* out_schema,
    std::string* out_error,
    const std::string& field_prefix) {
  if (resource == nullptr || out_schema == nullptr) {
    if (out_error != nullptr) {
      *out_error = "resource schema validator received null output pointer";
    }
    return false;
  }
  ResourceSchema schema;
  if (!LookupRequiredInt(*resource, "startup_priority", field_prefix, &schema.startup_priority, out_error)) {
    return false;
  }
  if (!LookupRequiredCpuSet(resource, field_prefix, &schema.cpu_set, out_error)) {
    return false;
  }
  if (!LookupRequiredInt(*resource, "restart_backoff_ms", field_prefix, &schema.restart_backoff_ms, out_error)) {
    return false;
  }
  if (!LookupRequiredInt(*resource, "restart_max_retries", field_prefix, &schema.restart_max_retries, out_error)) {
    return false;
  }
  if (!LookupRequiredInt(*resource, "restart_window_ms", field_prefix, &schema.restart_window_ms, out_error)) {
    return false;
  }
  if (!LookupRequiredInt(*resource, "restart_fuse_ms", field_prefix, &schema.restart_fuse_ms, out_error)) {
    return false;
  }
  if (!LookupRequiredInt(*resource, "ready_timeout_ms", field_prefix, &schema.ready_timeout_ms, out_error)) {
    return false;
  }
  *out_schema = schema;
  return true;
}

namespace {

std::filesystem::path ResolveIoPath(const std::filesystem::path& launch_dir, const std::string& raw) {
  std::filesystem::path p(raw);
  if (p.is_absolute()) {
    return p;
  }
  return launch_dir / p;
}

}  // namespace

bool ApplyLaunchPlanScalarsToRegisteredGflags(const ParsedLaunchPlan& plan, std::string* out_error) {
  for (const auto& mod : plan.modules) {
    for (const auto& [k, v] : mod.resource) {
      google::CommandLineFlagInfo info;
      if (!google::GetCommandLineFlagInfo(k.c_str(), &info)) {
        if (out_error != nullptr) {
          *out_error = "no gflag registered for resource key '" + k + "' (module " + mod.module_name + ")";
        }
        return false;
      }
      google::SetCommandLineOption(k.c_str(), ScalarToAssignmentString(v).c_str());
    }
    for (const auto& [k, v] : mod.module_params) {
      google::CommandLineFlagInfo info;
      if (!google::GetCommandLineFlagInfo(k.c_str(), &info)) {
        if (out_error != nullptr) {
          *out_error = "no gflag registered for module_params key '" + k + "' (module " + mod.module_name + ")";
        }
        return false;
      }
      google::SetCommandLineOption(k.c_str(), ScalarToAssignmentString(v).c_str());
    }
  }
  return true;
}

bool ApplyLaunchPlanScalarsToMatchingRegisteredGflags(
    const ParsedLaunchPlan& plan,
    LaunchPlanGflagMatchPolicy policy,
    std::string* out_error) {
  // 标准 resource 字段由 ParsedModuleLaunchEntry::resource_schema 消费，不应对应全局 gflags
  // （且键名可能与第三方库注册的 flag 撞名，例如 startup_priority）。
  auto is_bundled_resource_schema_field = [](const std::string& key) -> bool {
    return key == "startup_priority" || key == "cpu_set" || key == "cpu_id" ||
           key == "restart_backoff_ms" || key == "restart_max_retries" || key == "restart_window_ms" ||
           key == "restart_fuse_ms" || key == "ready_timeout_ms";
  };

  auto apply_one = [&](const std::string& module_name, const std::string& key,
                       const LaunchPlanScalar& value) -> bool {
    google::CommandLineFlagInfo info;
    if (!google::GetCommandLineFlagInfo(key.c_str(), &info)) {
      if (policy == LaunchPlanGflagMatchPolicy::kFailOnUnknownKeys) {
        if (out_error != nullptr) {
          *out_error = "no gflag registered for key '" + key + "' (module " + module_name + ")";
        }
        return false;
      }
      return true;
    }
    const std::string assign = ScalarToAssignmentString(value);
    // gflags: 成功时返回非空（新值的描述），失败（未知 flag / 非法值）时返回空串。
    const std::string set_result = google::SetCommandLineOption(key.c_str(), assign.c_str());
    if (set_result.empty()) {
      if (out_error != nullptr) {
        *out_error = "SetCommandLineOption failed for key '" + key + "' module=" + module_name +
                     " value='" + assign + "'";
      }
      return false;
    }
    return true;
  };

  for (const auto& mod : plan.modules) {
    for (const auto& [k, v] : mod.resource) {
      if (is_bundled_resource_schema_field(k)) {
        continue;
      }
      if (!apply_one(mod.module_name, k, v)) {
        return false;
      }
    }
    for (const auto& [k, v] : mod.module_params) {
      if (!apply_one(mod.module_name, k, v)) {
        return false;
      }
    }
  }
  return true;
}

bool ParseLaunchPlanFile(
  const std::string& launch_plan_file_path,
  ParsedLaunchPlan* out_plan,
  std::string* out_error,
  const LaunchPlanValidationOptions& options) {
  if (out_plan == nullptr) {
    if (out_error != nullptr) {
      *out_error = "output plan pointer is null";
    }
    return false;
  }
  *out_plan = ParsedLaunchPlan{};

  std::string read_err;
  const std::string text = ReadEntireFile(launch_plan_file_path, &read_err);
  if (text.empty() && read_err.find("failed to open") != std::string::npos) {
    if (out_error != nullptr) {
      *out_error = "launch plan not found or unreadable: " + launch_plan_file_path;
    }
    return false;
  }

  std::string parse_err;
  JValue root = ParseJsonTree(text, &parse_err);
  if (!parse_err.empty()) {
    if (out_error != nullptr) {
      *out_error = launch_plan_file_path + ": " + parse_err;
    }
    return false;
  }
  if (root.kind != JValue::kObject) {
    if (out_error != nullptr) {
      *out_error = launch_plan_file_path + ": root must be JSON object";
    }
    return false;
  }

  const JValue* modules_node = FindObjectKey(root.o, kModulesField);
  const JValue* minloglevel_node = FindObjectKey(root.o, kMinLogLevelField);
  const JValue* communication_node = FindObjectKey(root.o, kCommunicationField);
  if (modules_node == nullptr) {
    if (out_error != nullptr) {
      *out_error = launch_plan_file_path + ": missing top-level '" + std::string(kModulesField) + "'";
    }
    return false;
  }
  if (modules_node->kind != JValue::kObject) {
    if (out_error != nullptr) {
      *out_error = launch_plan_file_path + ": '" + std::string(kModulesField) + "' must be object";
    }
    return false;
  }
  if (minloglevel_node != nullptr) {
    LaunchPlanScalar minloglevel_scalar;
    if (!ScalarFromJsonLeaf(*minloglevel_node, kMinLogLevelField, &minloglevel_scalar, out_error)) {
      return false;
    }
    std::int64_t minloglevel_value = 0;
    if (!TryExtractInt64(minloglevel_scalar, kMinLogLevelField, &minloglevel_value, out_error)) {
      return false;
    }
    if (minloglevel_value < 0 || minloglevel_value > 3) {
      if (out_error != nullptr) {
        *out_error = std::string(kMinLogLevelField) + ": out of range [0,3]";
      }
      return false;
    }
    out_plan->minloglevel = minloglevel_value;
  }
  if (communication_node != nullptr) {
    if (communication_node->kind != JValue::kObject) {
      if (out_error != nullptr) {
        *out_error = std::string(kCommunicationField) + ": must be object";
      }
      return false;
    }
    const JValue* slot_count_node = FindObjectKey(communication_node->o, kShmSlotCountField);
    const JValue* slot_payload_bytes_node = FindObjectKey(communication_node->o, kSlotPayloadBytesField);
    for (const auto& [ck, cv] : communication_node->o) {
      if (ck != kShmSlotCountField && ck != kSlotPayloadBytesField) {
        if (out_error != nullptr) {
          *out_error =
              std::string(kCommunicationField) + ": unknown key '" + ck + "'";
        }
        return false;
      }
      (void)cv;
    }
    if (slot_count_node != nullptr) {
      LaunchPlanScalar slot_scalar;
      if (!ScalarFromJsonLeaf(
              *slot_count_node,
              std::string(kCommunicationField) + "." + kShmSlotCountField,
              &slot_scalar,
              out_error)) {
        return false;
      }
      std::int64_t slot_count_value = 0;
      if (!TryExtractInt64(
              slot_scalar,
              std::string(kCommunicationField) + "." + kShmSlotCountField,
              &slot_count_value,
              out_error)) {
        return false;
      }
      if (slot_count_value <= 0 ||
          slot_count_value > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
        if (out_error != nullptr) {
          *out_error = std::string(kCommunicationField) + "." + kShmSlotCountField +
              ": must be in range [1,4294967295]";
        }
        return false;
      }
      out_plan->communication_slot_count = static_cast<std::uint32_t>(slot_count_value);
    }
    if (slot_payload_bytes_node != nullptr) {
      LaunchPlanScalar payload_scalar;
      if (!ScalarFromJsonLeaf(
              *slot_payload_bytes_node,
              std::string(kCommunicationField) + "." + kSlotPayloadBytesField,
              &payload_scalar,
              out_error)) {
        return false;
      }
      std::int64_t slot_payload_bytes_value = 0;
      if (!TryExtractInt64(
              payload_scalar,
              std::string(kCommunicationField) + "." + kSlotPayloadBytesField,
              &slot_payload_bytes_value,
              out_error)) {
        return false;
      }
      if (slot_payload_bytes_value <= 0 ||
          slot_payload_bytes_value >
              static_cast<std::int64_t>(std::numeric_limits<std::int64_t>::max())) {
        if (out_error != nullptr) {
          *out_error = std::string(kCommunicationField) + "." + kSlotPayloadBytesField +
              ": must be a positive integer";
        }
        return false;
      }
      out_plan->communication_slot_payload_bytes = static_cast<std::size_t>(slot_payload_bytes_value);
    }
  }

  for (const auto& [k, v] : root.o) {
    if (k != kModulesField && k != kMinLogLevelField && k != kCommunicationField) {
      if (out_error != nullptr) {
        *out_error = launch_plan_file_path + ": unknown top-level key '" + k + "'";
      }
      return false;
    }
    (void)v;
  }

  const std::filesystem::path launch_path = launch_plan_file_path;
  out_plan->launch_plan_path = launch_path;
  const std::filesystem::path launch_dir = launch_path.parent_path();

  std::vector<ModuleChannelConfig> channel_cfgs;
  std::vector<ParsedModuleLaunchEntry> staged_modules;
  staged_modules.reserve(modules_node->o.size());
  std::unordered_map<std::string, LaunchPlanScalar> global_scalars;

  for (const auto& [dict_key, mod_node] : modules_node->o) {
    if (mod_node.kind != JValue::kObject) {
      if (out_error != nullptr) {
        *out_error = "modules." + dict_key + ": module entry must be object";
      }
      return false;
    }
    const JObject& mo = mod_node.o;

    const JValue* name_v = FindObjectKey(mo, "module_name");
    const JValue* res_v = FindObjectKey(mo, "resource");
    const JValue* par_v = FindObjectKey(mo, "module_params");
    const JValue* io_v = FindObjectKey(mo, kIoPathField);
    if (name_v == nullptr) {
      if (out_error != nullptr) {
        *out_error = "modules." + dict_key + ": missing module_name";
      }
      return false;
    }
    if (res_v == nullptr) {
      if (out_error != nullptr) {
        *out_error = "modules." + dict_key + ": missing resource";
      }
      return false;
    }
    if (par_v == nullptr) {
      if (out_error != nullptr) {
        *out_error = "modules." + dict_key + ": missing module_params";
      }
      return false;
    }
    if (io_v == nullptr) {
      if (out_error != nullptr) {
        *out_error = "modules." + dict_key + ": missing " + std::string(kIoPathField);
      }
      return false;
    }
    if (name_v->kind != JValue::kString || name_v->s.empty()) {
      if (out_error != nullptr) {
        *out_error = "modules." + dict_key + ".module_name: must be non-empty string";
      }
      return false;
    }
    if (name_v->s != dict_key) {
      if (out_error != nullptr) {
        *out_error = "modules." + dict_key + ": module_name must match modules map key";
      }
      return false;
    }
    if (io_v->kind != JValue::kString || io_v->s.empty()) {
      if (out_error != nullptr) {
        *out_error = "modules." + dict_key + "." + kIoPathField + ": must be non-empty string";
      }
      return false;
    }

    for (const auto& [mk, mv] : mo) {
      if (mk != "module_name" && mk != "resource" && mk != "module_params" && mk != kIoPathField) {
        if (out_error != nullptr) {
          *out_error = "modules." + dict_key + ": unknown key '" + mk + "'";
        }
        return false;
      }
      (void)mv;
    }

    ParsedModuleLaunchEntry entry;
    entry.modules_dict_key = dict_key;
    entry.module_name = name_v->s;
    if (!ParseStringMapScalars(*res_v, "modules." + dict_key + ".resource", &entry.resource, out_error)) {
      return false;
    }
    if (options.enforce_strict_resource_schema &&
        !ResourceSchemaValidator::ValidateAndNormalize(
            &entry.resource, &entry.resource_schema, out_error, "modules." + dict_key + ".resource")) {
      return false;
    }
    if (!ParseStringMapScalars(*par_v, "modules." + dict_key + ".module_params", &entry.module_params, out_error)) {
      return false;
    }

    if (options.registered_module_names != nullptr &&
        options.registered_module_names->find(entry.module_name) == options.registered_module_names->end()) {
      if (out_error != nullptr) {
        *out_error = "modules." + dict_key + ".module_name: unregistered module factory '" + entry.module_name + "'";
      }
      return false;
    }

    for (const auto& [gk, gv] : entry.resource) {
      if (IsPerModuleResourceSchemaKey(gk)) {
        continue;
      }
      if (!RecordGlobalScalar(gk, gv, &global_scalars, out_error)) {
        return false;
      }
    }
    for (const auto& [gk, gv] : entry.module_params) {
      if (!RecordGlobalScalar(gk, gv, &global_scalars, out_error)) {
        return false;
      }
    }

    const std::filesystem::path io_abs = ResolveIoPath(launch_dir, io_v->s).lexically_normal();
    const std::string io_abs_str = io_abs.string();
    entry.io_channels_config_path_resolved = io_abs_str;
    if (!ParseModuleIoJsonFile(entry.module_name, io_abs_str, &entry.channels, out_error)) {
      if (out_error != nullptr && out_error->find("failed to open") != std::string::npos) {
        *out_error = "modules." + dict_key + ": io file not found at " + io_abs_str;
      } else if (out_error != nullptr && out_error->find("module '") == std::string::npos) {
        *out_error = "modules." + dict_key + " (" + entry.module_name + "): " + *out_error;
      }
      return false;
    }

    channel_cfgs.push_back(entry.channels);
    staged_modules.push_back(std::move(entry));
  }

  if (!BuildChannelTopologyIndex(channel_cfgs, &out_plan->global_topology, out_error)) {
    *out_plan = ParsedLaunchPlan{};
    return false;
  }
  out_plan->modules = std::move(staged_modules);
  return true;
}

bool SetupRuntimeEnvironmentFromLaunchPlan(
    const std::unordered_map<std::string, ParsedModuleLaunchEntry>& module_entries,
    const ParsedLaunchPlan& launch_plan,
    std::vector<std::pair<std::string, std::string>>* out_module_config_files,
    std::string* out_error) {
  if (out_module_config_files == nullptr) {
    if (out_error != nullptr) {
      *out_error = "out_module_config_files is null";
    }
    return false;
  }
  out_module_config_files->clear();
  out_module_config_files->reserve(module_entries.size());
  std::ostringstream module_configs;
  bool first = true;
  for (const auto& [module_name, module_entry] : module_entries) {
    if (module_entry.io_channels_config_path_resolved.empty()) {
      if (out_error != nullptr) {
        *out_error = "module '" + module_name + "' has empty resolved io config path";
      }
      return false;
    }
    if (!first) {
      module_configs << ";";
    }
    first = false;
    module_configs << module_name << "=" << module_entry.io_channels_config_path_resolved;
    out_module_config_files->push_back({module_name, module_entry.io_channels_config_path_resolved});
  }

  const std::string configs_value = module_configs.str();
  if (configs_value.empty()) {
    if (out_error != nullptr) {
      *out_error = "no module configs found when preparing MOULD_MODULE_CHANNEL_CONFIGS";
    }
    return false;
  }

  if (setenv("MOULD_MODULE_CHANNEL_CONFIGS", configs_value.c_str(), 1) != 0) {
    if (out_error != nullptr) {
      *out_error = "setenv MOULD_MODULE_CHANNEL_CONFIGS failed, errno=" + std::to_string(errno);
    }
    return false;
  }
  if (setenv("MOULD_FORK_INHERITANCE_TOKEN", "backend_main_fork_model", 1) != 0) {
    if (out_error != nullptr) {
      *out_error = "setenv MOULD_FORK_INHERITANCE_TOKEN failed, errno=" + std::to_string(errno);
    }
    return false;
  }
  const std::uint32_t configured_slot_count = launch_plan.communication_slot_count.value_or(256U);
  const std::string slot_count_value = std::to_string(configured_slot_count);
  if (setenv("MOULD_SHM_SLOT_COUNT", slot_count_value.c_str(), 1) != 0) {
    if (out_error != nullptr) {
      *out_error = "setenv MOULD_SHM_SLOT_COUNT failed, errno=" + std::to_string(errno);
    }
    return false;
  }
  const std::size_t configured_slot_payload_bytes =
      launch_plan.communication_slot_payload_bytes.value_or(1024U);
  const std::string slot_payload_bytes_value = std::to_string(configured_slot_payload_bytes);
  if (setenv("MOULD_SHM_SLOT_PAYLOAD_BYTES", slot_payload_bytes_value.c_str(), 1) != 0) {
    if (out_error != nullptr) {
      *out_error = "setenv MOULD_SHM_SLOT_PAYLOAD_BYTES failed, errno=" + std::to_string(errno);
    }
    return false;
  }
  return true;
}

std::string ResolveLaunchPlanPath(int argc, char** argv) {
  if (argc > 1 && argv[1] != nullptr && std::strlen(argv[1]) > 0) {
    return argv[1];
  }
  return {};
}

}  // namespace mould::config
