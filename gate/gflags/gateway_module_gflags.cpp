#include "gate/gflags/gateway_module_gflags.hpp"

DEFINE_string(gateway_listen_addr, "0.0.0.0",
              "HttpGatewayModule module_params.gateway_listen_addr: listen address");
DEFINE_int32(gateway_port, 8080,
             "HttpGatewayModule module_params.gateway_port: listen port");
DEFINE_int32(gateway_max_connections, 10000,
             "HttpGatewayModule module_params.gateway_max_connections: max concurrent connections");
DEFINE_int32(gateway_max_body_size, 52428800,
             "HttpGatewayModule module_params.gateway_max_body_size: max body size (50 MB)");
DEFINE_double(gateway_max_rate, 10000.0,
              "HttpGatewayModule module_params.gateway_max_rate: max rate per connection (bytes/s)");
DEFINE_double(gateway_qps_limit, 10000.0,
              "HttpGatewayModule module_params.gateway_qps_limit: QPS limit per instance");
DEFINE_int32(gateway_io_threads, 4,
             "HttpGatewayModule module_params.gateway_io_threads: muduo IO thread count");
DEFINE_int32(gateway_timeout_ms, 5000,
             "HttpGatewayModule module_params.gateway_timeout_ms: request timeout (ms)");
DEFINE_int32(gateway_oss_retry, 3,
             "HttpGatewayModule module_params.gateway_oss_retry: OSS upload retry count");
