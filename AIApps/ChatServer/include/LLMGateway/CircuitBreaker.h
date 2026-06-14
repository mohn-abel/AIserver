#pragma once

#include <mutex>
#include <string>
#include "GatewayTypes.h"

// ---------------------------------------------------------------------------
// CircuitBreaker — 固定时间窗口熔断器，线程安全
//
// CLOSED:
//   在固定时间窗口内统计请求总数和失败数。窗口内请求数达到 minRequests 后，
//   如果失败率 >= failureRateThreshold，则进入 OPEN。
//
// OPEN:
//   直接拒绝请求。超过 recoveryTimeoutMs 后，下一次 allowRequest 会进入 HALF_OPEN。
//
// HALF_OPEN:
//   放行探活请求。任意一次失败会重新 OPEN；连续探活成功达到 halfOpenMax 后关闭熔断。
// ---------------------------------------------------------------------------
class CircuitBreaker {
public:
    CircuitBreaker() = default;

    // 配置参数。failureRateThreshold 取值范围为 0.0~1.0。
    void configure(long long windowMs,
                   double failureRateThreshold,
                   int minRequests,
                   long long recoveryTimeoutMs,
                   int halfOpenMax);

    // 请求前调用：返回 true 表示放行。
    // OPEN 状态下超过恢复时间后会进入 HALF_OPEN 并放行本次探活请求。
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
    void rotateWindowIfNeeded(long long now);
    void resetWindow(long long now);
    void recordClosedRequest(bool failed);

    std::string   backendId_;
    CircuitState  state_              = CircuitState::CLOSED;
    int           halfOpenSuccesses_   = 0; // 半开探活成功次数
    long long     openedTimeMs_        = 0; // 最近一次进入 OPEN 的时间

    long long     windowMs_             = 10000; // 固定统计窗口长度
    double        failureRateThreshold_ = 0.5;   // 失败率阈值，0.5 表示 50%
    int           minRequests_          = 5;     // 窗口内达到该请求数后才判断失败率
    int           windowRequests_       = 0;     // 当前窗口请求总数
    int           windowFailures_       = 0;     // 当前窗口失败数
    long long     windowStartMs_        = 0;     // 当前固定窗口开始时间

    long long     recoveryTimeoutMs_    = 30000; // OPEN 后多久允许探活
    int           halfOpenMaxRequests_  = 3;     // HALF_OPEN 中连续成功多少次后恢复 CLOSED

    mutable std::mutex mutex_;
};
