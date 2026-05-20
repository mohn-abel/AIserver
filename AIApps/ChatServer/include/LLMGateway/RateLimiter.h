#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include "GatewayTypes.h"

// ---------------------------------------------------------------------------
// TokenBucket — 平滑令牌桶，线程安全
// ---------------------------------------------------------------------------
class TokenBucket {
public:
    // rate: 每秒补充的令牌数  burst: 桶容量上限
    TokenBucket(double rate = 10.0, double burst = 5.0);

    // 尝试消费 1 个令牌，成功返回 true
    bool tryConsume();

    // 动态调整速率（用于配置热更新）
    void setRate(double rate, double burst);

private:
    void refill();

    double      rate_;              // 令牌/秒
    double      burstSize_;         // 桶容量
    double      tokens_;            // 当前令牌数
    long long   lastRefillMs_;      // 上次填充时间
    std::mutex  mutex_;
};

// ---------------------------------------------------------------------------
// RateLimiter — 两级限流（每后端 + 每用户）
// ---------------------------------------------------------------------------
class RateLimiter {
public:
    RateLimiter() = default;

    // 配置后端限流参数
    void configureBackend(const std::string& backendId, double rps, double burst);

    // 配置每用户限流参数
    void configureUserLimit(double rps, double burst);

    // 用户级限流：每个请求在入口处调用一次。抛 RateLimitException
    void checkUserAllowed(int userId);

    // 后端级限流：每次尝试后端时调用。抛 RateLimitException
    void checkBackendAllowed(const std::string& backendId);

private:
    // 后端级桶：backendId → TokenBucket
    std::unordered_map<std::string, TokenBucket> backendBuckets_;
    std::mutex backendMutex_;

    // 用户级桶：userId → TokenBucket
    std::unordered_map<int, TokenBucket> userBuckets_;
    std::mutex userMutex_;

    double userRps_    = 3.0;
    double userBurst_  = 2.0;
};
