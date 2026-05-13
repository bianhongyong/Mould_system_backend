#pragma once

#include <gflags/gflags.h>
#include <string>

DECLARE_string(gateway_listen_addr);
DECLARE_int32(gateway_port);
DECLARE_int32(gateway_max_connections);
DECLARE_int32(gateway_max_body_size);
DECLARE_double(gateway_max_rate);
DECLARE_double(gateway_qps_limit);
DECLARE_int32(gateway_io_threads);
DECLARE_int32(gateway_timeout_ms);
DECLARE_int32(gateway_oss_retry);
