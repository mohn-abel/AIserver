#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include "GatewayTypes.h"

// ---------------------------------------------------------------------------
// CircuitBreaker — 连续失败阈值 + 时间衰减窗口熔断器，线程安全
//
// CLOSED:
//   统计连续失败次数。一次成功不会立即清零；只有距离最后一次失败超过
//   failureResetTimeoutMs 后，下一次成功才会重置失败计数，避免
//   "失败-成功-失败-成功" 交替模式下熔断失效。
//
// OPEN:
//   直接拒绝请求。超过 recoveryTimeoutMs 后，下一次 allowRequest 会尝试
//   通过 CAS 进入 HALF_OPEN。
//
// HALF_OPEN:
//   限制本轮探活尝试次数。任意一次探活失败会重新 OPEN；
//   探活成功达到 successThreshold 后关闭熔断。
// ---------------------------------------------------------------------------
class CircuitBreaker {
public:
    CircuitBreaker() = default;
    using Permit = std::uint64_t;

    void configure(int failureThreshold,
                   long long failureResetTimeoutMs,
                   long long recoveryTimeoutMs,
                   int halfOpenMaxCalls,
                   int successThreshold);

    // 请求前调用：有返回值表示放行；完成后必须携带 permit 上报一次。
    std::optional<Permit> allowRequest();

    // 请求成功后调用
    void reportSuccess(Permit permit);

    // 请求失败后调用
    void reportFailure(Permit permit);

    // 查询当前状态
    CircuitState state() const;

    // 返回当前后端 ID（调试用）。初始化阶段调用，不参与并发状态机。
    void setBackendId(const std::string& id) { backendId_ = id; }
    const std::string& backendId() const { return backendId_; }

private:
    std::string backendId_;

    mutable std::mutex mutex_;

    int       failureThreshold_       = 5;
    long long failureResetTimeoutMs_  = 10000;
    long long recoveryTimeoutMs_      = 30000;
    int       halfOpenMaxCalls_       = 2;
    int       successThreshold_       = 2;

    CircuitState state_               = CircuitState::CLOSED;
    Permit       generation_          = 0;
    int          consecutiveFailures_ = 0;
    long long    openedTimeMs_        = 0;
    int          halfOpenCalls_       = 0;
    int          halfOpenSuccesses_   = 0;
    long long    lastFailureTimeMs_   = 0;
};
