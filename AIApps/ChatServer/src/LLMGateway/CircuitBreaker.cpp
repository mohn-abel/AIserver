#include "../../include/LLMGateway/CircuitBreaker.h"
#include <algorithm>
#include <iostream>
// 熔断器配置
void CircuitBreaker::configure(long long windowMs,
                               double failureRateThreshold,
                               int minRequests,
                               long long recoveryTimeoutMs,
                               int halfOpenMax) {
    std::lock_guard<std::mutex> lock(mutex_);                     
    windowMs_ = std::max(1LL, windowMs);
    failureRateThreshold_ = std::clamp(failureRateThreshold, 0.0, 1.0);
    minRequests_ = std::max(1, minRequests);
    recoveryTimeoutMs_ = recoveryTimeoutMs;
    halfOpenMaxRequests_ = std::max(1, halfOpenMax);

    resetWindow(nowMs());
    halfOpenSuccesses_ = 0;
}
// 重置窗口
void CircuitBreaker::resetWindow(long long now) {
    windowStartMs_ = now;
    windowRequests_ = 0;
    windowFailures_ = 0;
}

void CircuitBreaker::rotateWindowIfNeeded(long long now) {
    if (windowStartMs_ == 0 || now - windowStartMs_ >= windowMs_) {
        resetWindow(now);
    }
}
// 状态流转
void CircuitBreaker::transitionTo(CircuitState newState) {
    CircuitState old = state_;
    if (old == newState) return;

    state_ = newState;
    long long now = nowMs();

    if (newState == CircuitState::OPEN) {
        openedTimeMs_ = now;
    }
    if (newState == CircuitState::HALF_OPEN) {
        halfOpenSuccesses_ = 0;
    }
    if (newState == CircuitState::CLOSED) {
        halfOpenSuccesses_ = 0;
        resetWindow(now);
    }

    std::cout << "[CircuitBreaker:" << backendId_ << "] "
              << circuitStateName(old) << " -> " << circuitStateName(newState)
              << " (window=" << windowFailures_ << "/" << windowRequests_
              << ", threshold=" << failureRateThreshold_ << ")" << std::endl;
}
// 请求通过
bool CircuitBreaker::allowRequest() {
    std::lock_guard<std::mutex> lock(mutex_);

    switch (state_) {
    case CircuitState::CLOSED:
        rotateWindowIfNeeded(nowMs());
        return true;

    case CircuitState::OPEN: {
        long long elapsed = nowMs() - openedTimeMs_;
        if (elapsed >= recoveryTimeoutMs_) {
            transitionTo(CircuitState::HALF_OPEN);
            return true;
        }
        return false;
    }

    case CircuitState::HALF_OPEN:
        return true;
    }

    return false;
}

void CircuitBreaker::recordClosedRequest(bool failed) {
    long long now = nowMs();
    rotateWindowIfNeeded(now);

    windowRequests_++;
    if (failed) {
        windowFailures_++;
    }

    if (windowRequests_ < minRequests_) {
        return;
    }

    double failureRate = static_cast<double>(windowFailures_) / windowRequests_;
    if (failureRate >= failureRateThreshold_) {
        transitionTo(CircuitState::OPEN);
    }
}

void CircuitBreaker::reportSuccess() {
    std::lock_guard<std::mutex> lock(mutex_);

    switch (state_) {
    case CircuitState::CLOSED:
        recordClosedRequest(false);
        break;

    case CircuitState::HALF_OPEN:
        halfOpenSuccesses_++;
        if (halfOpenSuccesses_ >= halfOpenMaxRequests_) {
            transitionTo(CircuitState::CLOSED);
        }
        break;

    case CircuitState::OPEN:
        break;
    }
}

void CircuitBreaker::reportFailure() {
    std::lock_guard<std::mutex> lock(mutex_);

    switch (state_) {
    case CircuitState::CLOSED:
        recordClosedRequest(true);
        break;

    case CircuitState::HALF_OPEN:
        transitionTo(CircuitState::OPEN);
        break;

    case CircuitState::OPEN:
        break;
    }
}

CircuitState CircuitBreaker::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}
