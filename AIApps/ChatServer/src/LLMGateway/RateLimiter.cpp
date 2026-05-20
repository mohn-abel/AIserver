#include "../../include/LLMGateway/RateLimiter.h"
#include <algorithm>
#include <cmath>

// ============================================================================
// TokenBucket
// ============================================================================

TokenBucket::TokenBucket(double rate, double burst)
    : rate_(rate), burstSize_(burst), tokens_(burst), lastRefillMs_(nowMs()) {}

void TokenBucket::refill() {
    long long now = nowMs();
    long long elapsed = now - lastRefillMs_;
    if (elapsed <= 0) return;

    // 按时间比例补充令牌
    double added = (elapsed / 1000.0) * rate_;
    tokens_ = std::min(tokens_ + added, burstSize_);
    lastRefillMs_ = now;
}

bool TokenBucket::tryConsume() {
    std::lock_guard<std::mutex> lock(mutex_);
    refill();
    if (tokens_ >= 1.0) {
        tokens_ -= 1.0;
        return true;
    }
    return false;
}

void TokenBucket::setRate(double rate, double burst) {
    std::lock_guard<std::mutex> lock(mutex_);
    rate_ = rate;
    burstSize_ = burst;
    if (tokens_ > burstSize_) tokens_ = burstSize_;
}

// ============================================================================
// RateLimiter
// ============================================================================

void RateLimiter::configureBackend(const std::string& backendId, double rps, double burst) {
    std::lock_guard<std::mutex> lock(backendMutex_);
    auto it = backendBuckets_.find(backendId);
    if (it != backendBuckets_.end()) {
        it->second.setRate(rps, burst);
    } else {
        backendBuckets_.emplace(std::piecewise_construct,
                                std::forward_as_tuple(backendId),
                                std::forward_as_tuple(rps, burst));
    }
}

void RateLimiter::configureUserLimit(double rps, double burst) {
    userRps_   = rps;
    userBurst_ = burst;
}

void RateLimiter::checkUserAllowed(int userId) {
    if (userId <= 0 || userRps_ <= 0) return;

    std::lock_guard<std::mutex> lock(userMutex_);
    auto [userIt, userInserted] = userBuckets_.try_emplace(userId, userRps_, userBurst_);
    if (!userIt->second.tryConsume()) {
        throw RateLimitException(
            "Per-user rate limit exceeded for user " + std::to_string(userId));
    }
}

void RateLimiter::checkBackendAllowed(const std::string& backendId) {
    std::lock_guard<std::mutex> lock(backendMutex_);
    auto it = backendBuckets_.find(backendId);
    if (it == backendBuckets_.end()) {
        return;  // 未显式配置 → 不限流
    }
    if (!it->second.tryConsume()) {
        throw RateLimitException(
            "Rate limit exceeded for backend: " + backendId);
    }
}
