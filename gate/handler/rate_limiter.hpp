#pragma once

#include <algorithm>
#include <chrono>
#include <mutex>

namespace mould {
namespace gate {
namespace handler {

/// 限流器接口
class IRateLimiter {
public:
    virtual ~IRateLimiter() = default;

    /// 尝试获取一个令牌
    /// @return true 表示允许通过，false 表示限流
    virtual bool Acquire() = 0;

    /// 带显式时间戳的获取（用于外部驱动时间）
    /// @param now_ms 当前时间戳（毫秒）
    virtual bool Acquire(int64_t now_ms) {
        (void)now_ms;
        return Acquire();
    }
};

/// 基于令牌桶的限流器
///
/// 支持配置速率（tokens/sec）和 burst 上限。
/// 线程安全。
class TokenBucketRateLimiter : public IRateLimiter {
public:
    /// @param rate   令牌生成速率（个/秒），必须 > 0，否则 clamp 为 1.0
    /// @param burst  桶容量上限（最大令牌数），必须 > 0，否则 clamp 为 1.0
    TokenBucketRateLimiter(double rate, double burst)
        : rate_(rate > 0.0 ? rate : 1.0)
        , burst_(burst > 0.0 ? burst : 1.0)
        , tokens_(burst_)
        , last_refill_ms_(SteadyClockMs()) {}

    bool Acquire() override {
        return Acquire(SteadyClockMs());
    }

    bool Acquire(int64_t now_ms) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Refill(now_ms);
        if (tokens_ >= 1.0) {
            tokens_ -= 1.0;
            return true;
        }
        return false;
    }

    /// 获取当前令牌数（近似值，仅用于监控/测试）
    double Tokens() const {
        std::lock_guard<std::mutex> lock(mutex_);
        // 重新计算以确保精确
        int64_t now = SteadyClockMs();
        if (now > last_refill_ms_) {
            double elapsed = (now - last_refill_ms_) / 1000.0;
            return std::min(tokens_ + elapsed * rate_, burst_);
        }
        return tokens_;
    }

    /// 重置（清空令牌并重新配置），非线程安全，建议在单线程环境中使用
    void Reset(double rate, double burst) {
        std::lock_guard<std::mutex> lock(mutex_);
        rate_ = rate > 0.0 ? rate : 1.0;
        burst_ = burst > 0.0 ? burst : 1.0;
        tokens_ = burst_;
        last_refill_ms_ = SteadyClockMs();
    }

private:
    /// 补充令牌
    void Refill(int64_t now_ms) {
        if (now_ms < last_refill_ms_) {
            // 时钟回退或使用显式时间戳：重置基准时间
            last_refill_ms_ = now_ms;
            return;
        }
        if (now_ms == last_refill_ms_) {
            return;
        }
        double elapsed = (now_ms - last_refill_ms_) / 1000.0;
        tokens_ = std::min(tokens_ + elapsed * rate_, burst_);
        last_refill_ms_ = now_ms;
    }

    /// 获取当前 steady_clock 时间（毫秒）
    static int64_t SteadyClockMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    double rate_;
    double burst_;
    double tokens_;
    mutable int64_t last_refill_ms_;
    mutable std::mutex mutex_;
};

}  // namespace handler
}  // namespace gate
}  // namespace mould
