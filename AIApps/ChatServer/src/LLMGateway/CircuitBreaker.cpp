#include "../../include/LLMGateway/CircuitBreaker.h"
#include <algorithm>
#include <iostream>

void CircuitBreaker::configure(int failureThreshold,
                               long long failureResetTimeoutMs,
                               long long recoveryTimeoutMs,
                               int halfOpenMaxCalls,
                               int successThreshold) {
    std::lock_guard<std::mutex> lock(mutex_);

    failureThreshold_ = std::max(1, failureThreshold);
    failureResetTimeoutMs_ = std::max(1LL, failureResetTimeoutMs);
    recoveryTimeoutMs_ = std::max(0LL, recoveryTimeoutMs);
    halfOpenMaxCalls_ = std::max(1, halfOpenMaxCalls);
    successThreshold_ = std::min(std::max(1, successThreshold), halfOpenMaxCalls_);

    state_ = CircuitState::CLOSED;
    consecutiveFailures_ = 0;
    openedTimeMs_ = 0;
    halfOpenCalls_ = 0;
    halfOpenSuccesses_ = 0;
    lastFailureTimeMs_ = 0;
    ++generation_;
}

std::optional<CircuitBreaker::Permit> CircuitBreaker::allowRequest() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (state_ == CircuitState::CLOSED) {
        return generation_;
    }

    if (state_ == CircuitState::OPEN) {
        auto now = nowMs();
        if (now - openedTimeMs_ >= recoveryTimeoutMs_) {
            state_ = CircuitState::HALF_OPEN;
            ++generation_;
            halfOpenCalls_ = 0;
            halfOpenSuccesses_ = 0;
            std::cout << "[CircuitBreaker:" << backendId_
                      << "] OPEN -> HALF_OPEN" << std::endl;
        } else {
            return std::nullopt;
        }
    }

    if (state_ == CircuitState::HALF_OPEN) {
        if (halfOpenCalls_ >= halfOpenMaxCalls_) {
            return std::nullopt;
        }
        ++halfOpenCalls_;
        return generation_;
    }

    return std::nullopt;
}

void CircuitBreaker::reportSuccess(Permit permit) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (permit != generation_) {
        return;
    }

    if (state_ == CircuitState::CLOSED) {
        // 成功不立即清零失败计数；只有失败窗口过期后才清零。
        // 这样可以避免 F,F,F,F,S,F 这类交替模式绕过熔断。
        auto now = nowMs();
        auto lastFailure = lastFailureTimeMs_;
        if (lastFailure == 0 || now - lastFailure >= failureResetTimeoutMs_) {
            consecutiveFailures_ = 0;
        }
        return;
    }

    if (state_ == CircuitState::HALF_OPEN) {
        ++halfOpenSuccesses_;
        if (halfOpenSuccesses_ >= successThreshold_ &&
            halfOpenSuccesses_ == halfOpenCalls_) {
            state_ = CircuitState::CLOSED;
            ++generation_;
            consecutiveFailures_ = 0;
            lastFailureTimeMs_ = 0;
            openedTimeMs_ = 0;
            halfOpenCalls_ = 0;
            halfOpenSuccesses_ = 0;
            std::cout << "[CircuitBreaker:" << backendId_
                      << "] HALF_OPEN -> CLOSED (recovered)" << std::endl;
        }
    }
}

void CircuitBreaker::reportFailure(Permit permit) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (permit != generation_) {
        return;
    }

    auto now = nowMs();

    if (state_ == CircuitState::HALF_OPEN) {
        openedTimeMs_ = now;
        halfOpenCalls_ = 0;
        halfOpenSuccesses_ = 0;
        state_ = CircuitState::OPEN;
        ++generation_;
        std::cout << "[CircuitBreaker:" << backendId_
                  << "] HALF_OPEN probe failed -> OPEN" << std::endl;
        return;
    }

    if (state_ != CircuitState::CLOSED) {
        return;
    }

    auto lastFailure = lastFailureTimeMs_;
    if (lastFailure > 0 && now - lastFailure >= failureResetTimeoutMs_) {
        consecutiveFailures_ = 0;
    }

    lastFailureTimeMs_ = now;
    auto failures = ++consecutiveFailures_;
    if (failures >= failureThreshold_) {
        openedTimeMs_ = now;
        halfOpenCalls_ = 0;
        halfOpenSuccesses_ = 0;
        state_ = CircuitState::OPEN;
        ++generation_;
        std::cout << "[CircuitBreaker:" << backendId_
                  << "] CLOSED -> OPEN (failures=" << failures
                  << ", threshold=" << failureThreshold_ << ")" << std::endl;
    }
}

CircuitState CircuitBreaker::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}
