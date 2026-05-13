#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <glog/logging.h>

namespace mould {
namespace gate {
namespace handler {

/// 单请求指标
struct RequestMetrics {
    std::string request_id;
    std::string node_id;
    int http_status_code = 0;
    int64_t latency_ms = 0;
    bool shm_publish_success = false;
    std::string oss_result;  // "success" / "failed" / "skipped"
};

/// 指标记录器接口
class IMetricsRecorder {
public:
    virtual ~IMetricsRecorder() = default;

    /// 记录一次请求的指标
    virtual void RecordRequest(const RequestMetrics& metrics) = 0;
};

/// 基于 glog 的指标记录器实现
///
/// 使用 glog 记录每请求指标，并维护累计计数器：总请求数、成功数、失败数。
class LoggingMetricsRecorder : public IMetricsRecorder {
public:
    LoggingMetricsRecorder() = default;

    // 禁止拷贝
    LoggingMetricsRecorder(const LoggingMetricsRecorder&) = delete;
    LoggingMetricsRecorder& operator=(const LoggingMetricsRecorder&) = delete;

    void RecordRequest(const RequestMetrics& metrics) override {
        total_requests_++;

        // 记录每请求日志
        LOG(INFO) << "[METRICS] request_id=" << metrics.request_id
                  << " node_id=" << metrics.node_id
                  << " status=" << metrics.http_status_code
                  << " latency_ms=" << metrics.latency_ms
                  << " shm=" << (metrics.shm_publish_success ? "ok" : "fail")
                  << " oss=" << metrics.oss_result;

        // 更新分类计数器
        if (metrics.http_status_code >= 200 && metrics.http_status_code < 300) {
            success_requests_++;
        } else {
            failed_requests_++;
        }

        // 更新延迟统计
        int64_t prev_total = total_latency_ms_.load(std::memory_order_relaxed);
        total_latency_ms_.store(prev_total + metrics.latency_ms,
                                std::memory_order_relaxed);
    }

    /// 获取总请求数
    uint64_t TotalRequests() const {
        return total_requests_.load(std::memory_order_relaxed);
    }

    /// 获取成功请求数（2xx）
    uint64_t SuccessRequests() const {
        return success_requests_.load(std::memory_order_relaxed);
    }

    /// 获取失败请求数（非 2xx）
    uint64_t FailedRequests() const {
        return failed_requests_.load(std::memory_order_relaxed);
    }

    /// 获取累计延迟（毫秒）
    int64_t TotalLatencyMs() const {
        return total_latency_ms_.load(std::memory_order_relaxed);
    }

    /// 获取平均延迟（毫秒），无请求时返回 0
    double AvgLatencyMs() const {
        auto total = total_requests_.load(std::memory_order_relaxed);
        if (total == 0) {
            return 0.0;
        }
        return static_cast<double>(
                   total_latency_ms_.load(std::memory_order_relaxed)) /
               static_cast<double>(total);
    }

private:
    std::atomic<uint64_t> total_requests_{0};
    std::atomic<uint64_t> success_requests_{0};
    std::atomic<uint64_t> failed_requests_{0};
    std::atomic<int64_t> total_latency_ms_{0};
};

}  // namespace handler
}  // namespace gate
}  // namespace mould
