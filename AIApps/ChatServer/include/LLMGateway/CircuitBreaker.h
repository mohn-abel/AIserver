#pragma once

#include <mutex>
#include <string>
#include "GatewayTypes.h"

// ---------------------------------------------------------------------------
// CircuitBreaker — 三态熔断器，线程安全
// ---------------------------------------------------------------------------
class CircuitBreaker {
public:
    CircuitBreaker() = default;

    // 配置参数
    void configure(int failureThreshold, long long recoveryTimeoutMs, int halfOpenMax);

    // 请求前调用：返回 true 表示放行
    // HALF_OPEN 状态下会递增探测计数
    bool allowRequest();

    // 请求成功后调用
    void reportSuccess();

    // 请求失败后调用
    void reportFailure();

    // 查询当前状态
    CircuitState state() const;

    // 返回当前后端 ID（调试用）
    void setBackendId(const std::string& id) { backendId_ = id; }
    const std::string& backendId() const { return backendId_; }

private:
    void transitionTo(CircuitState newState);

    std::string   backendId_;
    CircuitState  state_              = CircuitState::CLOSED;
    int           consecutiveFailures_ = 0;
    int           halfOpenSuccesses_   = 0;
    long long     lastFailureTimeMs_   = 0;

    int           failureThreshold_    = 5;
    long long     recoveryTimeoutMs_   = 30000;
    int           halfOpenMaxRequests_ = 3;

    mutable std::mutex mutex_;
};
