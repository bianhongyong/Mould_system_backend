#include "gate/handler/counter_handler.hpp"

namespace mould {
namespace gate {
namespace handler {

CounterHandler::CounterHandler() = default;

CounterHandler::ParseResult CounterHandler::DoParse(
    http::RequestView& /*view*/) {
  ParseResult result;
  // 无需解析请求体，所有方法（GET/POST）都直接通过
  result.ok = true;
  result.status_code = 200;
  return result;
}

CounterHandler::ProcessResult CounterHandler::DoProcess(
    const ParseResult& /*input*/) {
  ProcessResult result;

  uint64_t count = counter_.fetch_add(1) + 1;

  result.ok = true;
  result.status_code = 200;
  result.request_id = std::to_string(count);
  result.resource_id = std::to_string(count);
  return result;
}

}  // namespace handler
}  // namespace gate
}  // namespace mould
