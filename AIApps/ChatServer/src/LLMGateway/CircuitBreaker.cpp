#include "../../include/LLMGateway/CircuitBreaker.h"
#include <algorithm>
#include <iostream>

void CircuitBreaker::configure(int failureThreshold,
                               long long failureResetTimeoutMs,
                               long long recoveryTimeoutMs,
                               int halfOpenMaxCalls,
                               int successThreshold) {
    failureThreshold_ = std::max(1, failureThreshold);
    failureResetTimeoutMs_ = std::max(1LL, failureResetTimeoutMs);
    recoveryTimeoutMs_ = std::max(0LL, recoveryTimeoutMs);
    halfOpenMaxCalls_ = std::max(1, halfOpenMaxCalls);
    successThreshold_ = std::max(1, successThreshold);

    state_.store(CircuitState::CLOSED);
    consecutiveFailures_.store(0);
    openedTimeMs_.store(0);
    halfOpenCalls_.store(0);
    halfOpenSuccesses_.store(0);
    lastFailureTimeMs_.store(0);
}

bool CircuitBreaker::allowRequest() {
    auto state = state_.load();

    if (state == CircuitState::CLOSED) {
        return true;
    }

    if (state == CircuitState::OPEN) {
        auto now = nowMs();
        if (now - openedTimeMs_.load() >= recoveryTimeoutMs_) {
            CircuitState expected = CircuitState::OPEN;
            if (state_.compare_exchange_strong(expected, CircuitState::HALF_OPEN)) {
                halfOpenCalls_.store(0);
                halfOpenSuccesses_.store(0);
                std::cout << "[CircuitBreaker:" << backendId_
                          << "] OPEN -> HALF_OPEN" << std::endl;
            }
            auto calls = halfOpenCalls_.fetch_add(1);
            return calls < halfOpenMaxCalls_;
        }
        return false;
    }

    if (state == CircuitState::HALF_OPEN) {
        auto calls = halfOpenCalls_.fetch_add(1);
        return calls < halfOpenMaxCalls_;
    }

    return false;
}

void CircuitBreaker::reportSuccess() {
    auto state = state_.load();

    if (state == CircuitState::CLOSED) {
        // 成功不立即清零失败计数；只有失败窗口过期后才清零。
        // 这样可以避免 F,F,F,F,S,F 这类交替模式绕过熔断。
        auto now = nowMs();
        auto lastFailure = lastFailureTimeMs_.load();
        if (lastFailure == 0 || now - lastFailure >= failureResetTimeoutMs_) {
            consecutiveFailures_.store(0);
        }
        return;
    }

    if (state == CircuitState::HALF_OPEN) {
        auto successes = halfOpenSuccesses_.fetch_add(1) + 1;
        if (successes >= successThreshold_) {
            state_.store(CircuitState::CLOSED);
            consecutiveFailures_.store(0);
            lastFailureTimeMs_.store(0);
            openedTimeMs_.store(0);
            halfOpenCalls_.store(0);
            halfOpenSuccesses_.store(0);
            std::cout << "[CircuitBreaker:" << backendId_
                      << "] HALF_OPEN -> CLOSED (recovered)" << std::endl;
        }
    }
}

void CircuitBreaker::reportFailure() {
    auto state = state_.load();
    auto now = nowMs();

    if (state == CircuitState::HALF_OPEN) {
        openedTimeMs_.store(now);
        halfOpenCalls_.store(0);
        halfOpenSuccesses_.store(0);
        state_.store(CircuitState::OPEN);
        std::cout << "[CircuitBreaker:" << backendId_
                  << "] HALF_OPEN probe failed -> OPEN" << std::endl;
        return;
    }

    if (state != CircuitState::CLOSED) {
        return;
    }

    auto lastFailure = lastFailureTimeMs_.load();
    if (lastFailure > 0 && now - lastFailure >= failureResetTimeoutMs_) {
        consecutiveFailures_.store(0);
    }

    lastFailureTimeMs_.store(now);
    auto failures = consecutiveFailures_.fetch_add(1) + 1;
    if (failures >= failureThreshold_) {
        state_.store(CircuitState::OPEN);
        openedTimeMs_.store(now);
        halfOpenCalls_.store(0);
        halfOpenSuccesses_.store(0);
        std::cout << "[CircuitBreaker:" << backendId_
                  << "] CLOSED -> OPEN (failures=" << failures
                  << ", threshold=" << failureThreshold_ << ")" << std::endl;
    }
}

CircuitState CircuitBreaker::state() const {
    return state_.load();
}
