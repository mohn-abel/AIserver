#include "../../include/LLMGateway/CircuitBreaker.h"
#include <iostream>

void CircuitBreaker::configure(int failureThreshold, long long recoveryTimeoutMs, int halfOpenMax) {
    std::lock_guard<std::mutex> lock(mutex_);
    failureThreshold_   = failureThreshold;
    recoveryTimeoutMs_  = recoveryTimeoutMs;
    halfOpenMaxRequests_ = halfOpenMax;
}

void CircuitBreaker::transitionTo(CircuitState newState) {
    CircuitState old = state_;
    state_ = newState;
    if (newState == CircuitState::OPEN) {
        lastFailureTimeMs_ = nowMs();
    }
    if (newState == CircuitState::HALF_OPEN) {
        halfOpenSuccesses_ = 0;
    }
    if (newState == CircuitState::CLOSED) {
        consecutiveFailures_ = 0;
    }
    std::cout << "[CircuitBreaker:" << backendId_ << "] "
              << circuitStateName(old) << " -> " << circuitStateName(newState)
              << " (failures=" << consecutiveFailures_ << ")" << std::endl;
}

bool CircuitBreaker::allowRequest() {
    std::lock_guard<std::mutex> lock(mutex_);

    switch (state_) {
    case CircuitState::CLOSED:
        return true;

    case CircuitState::OPEN: {
        long long elapsed = nowMs() - lastFailureTimeMs_;
        if (elapsed >= recoveryTimeoutMs_) {
            transitionTo(CircuitState::HALF_OPEN);
            return true;
        }
        return false;
    }

    case CircuitState::HALF_OPEN:
        // 半开状态放行所有请求作为探测
        return true;
    }

    return false;
}

void CircuitBreaker::reportSuccess() {
    std::lock_guard<std::mutex> lock(mutex_);

    switch (state_) {
    case CircuitState::CLOSED:
        consecutiveFailures_ = 0;
        break;

    case CircuitState::HALF_OPEN:
        halfOpenSuccesses_++;
        if (halfOpenSuccesses_ >= halfOpenMaxRequests_) {
            transitionTo(CircuitState::CLOSED);
        }
        break;

    case CircuitState::OPEN:
        // 不应到达此处（OPEN 状态下不放行请求）
        break;
    }
}

void CircuitBreaker::reportFailure() {
    std::lock_guard<std::mutex> lock(mutex_);

    switch (state_) {
    case CircuitState::CLOSED:
        consecutiveFailures_++;
        if (consecutiveFailures_ >= failureThreshold_) {
            transitionTo(CircuitState::OPEN);
        }
        break;

    case CircuitState::HALF_OPEN:
        // 半开状态下任何失败立即重新熔断
        consecutiveFailures_ = failureThreshold_;  // 保留失败计数
        transitionTo(CircuitState::OPEN);
        break;

    case CircuitState::OPEN:
        // 不应到达此处
        break;
    }
}

CircuitState CircuitBreaker::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}
