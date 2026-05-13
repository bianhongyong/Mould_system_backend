#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <string>

namespace mould {
namespace gate {
namespace handler {

/// 生成请求 ID
///
/// 格式: "gw-{timestamp}-{random_hex}{sequence_hex}"
/// 使用 std::random_device 生成随机部分，
/// 包含单调递增序列号保证在同一进程内不重复。
inline std::string GenerateRequestId() {
    static std::atomic<uint64_t> global_seq{0};
    static thread_local std::mt19937 rng(std::random_device{}());
    static thread_local std::uniform_int_distribution<int> hex_dist(0, 15);

    uint64_t seq = global_seq.fetch_add(1, std::memory_order_relaxed);

    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch())
                  .count();

    std::ostringstream oss;
    oss << "gw-" << ms << "-";

    // 序列号部分 (4 位十六进制) - 在随机部分前面以保证字符串有序
    oss << std::hex << std::setw(4) << std::setfill('0')
        << (seq & 0xFFFF);

    // 随机十六进制部分 (8 位)
    for (int i = 0; i < 8; ++i) {
        oss << std::hex << hex_dist(rng);
    }

    return oss.str();
}

}  // namespace handler
}  // namespace gate
}  // namespace mould
