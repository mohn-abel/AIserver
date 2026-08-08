#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <chrono>

// ---------------------------------------------------------------------------
// 异常体系 — 全部继承 std::runtime_error，兼容现有 catch(const std::exception&)
// ---------------------------------------------------------------------------
// 网关异常
class GatewayException : public std::runtime_error {
public:
    explicit GatewayException(const std::string& msg) : std::runtime_error(msg) {}
};
// 限流异常
class RateLimitException : public GatewayException {
public:
    explicit RateLimitException(const std::string& msg) : GatewayException(msg) {}
};
// 熔断异常
class CircuitOpenException : public GatewayException {
public:
    explicit CircuitOpenException(const std::string& msg) : GatewayException(msg) {}
};
// 超时异常
class TimeoutException : public GatewayException {
public:
    explicit TimeoutException(const std::string& msg) : GatewayException(msg) {}
};
// 后端异常
class BackendException : public GatewayException {
public:
    explicit BackendException(const std::string& msg) : GatewayException(msg) {}
};

// 后端 HTTP 错误语义。网关治理关心的是事件语义，不是裸状态码。
enum class BackendErrorClass {
    kClientRequest,      // 400/404/422: 请求体、模型名或路径问题
    kAuth,               // 401/403: 密钥、权限或账号问题
    kRateLimited,        // 429: 远端限流/过载
    kBackendUnavailable, // 408/5xx: 远端不可用或超时
    kUnknown
};

inline const char* backendErrorClassName(BackendErrorClass c) {
    switch (c) {
        case BackendErrorClass::kClientRequest:      return "client_request";
        case BackendErrorClass::kAuth:               return "auth";
        case BackendErrorClass::kRateLimited:        return "rate_limited";
        case BackendErrorClass::kBackendUnavailable: return "backend_unavailable";
        case BackendErrorClass::kUnknown:            return "unknown";
    }
    return "unknown";
}

class BackendHttpException : public BackendException {
public:
    BackendHttpException(int statusCode,
                         BackendErrorClass errorClass,
                         const std::string& msg)
        : BackendException(msg),
          statusCode_(statusCode),
          errorClass_(errorClass) {}

    int statusCode() const { return statusCode_; }
    BackendErrorClass errorClass() const { return errorClass_; }

    bool countsForCircuitBreaker() const {
        return errorClass_ == BackendErrorClass::kRateLimited ||
               errorClass_ == BackendErrorClass::kBackendUnavailable ||
               errorClass_ == BackendErrorClass::kUnknown;
    }

    bool shouldTryFallback() const {
        return countsForCircuitBreaker();
    }

private:
    int statusCode_;
    BackendErrorClass errorClass_;
};

// ---------------------------------------------------------------------------
// 熔断器状态
// ---------------------------------------------------------------------------

enum class CircuitState {
    CLOSED,      // 正常通行
    OPEN,        // 熔断，直接拒绝
    HALF_OPEN    // 探测恢复
};

inline const char* circuitStateName(CircuitState s) {
    switch (s) {
        case CircuitState::CLOSED:    return "CLOSED";
        case CircuitState::OPEN:      return "OPEN";
        case CircuitState::HALF_OPEN: return "HALF_OPEN";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// 后端配置快照（来自 gateway_config.json）
// ---------------------------------------------------------------------------

struct BackendConfig {
    std::string id;           // "aliyun-qwen"
    std::string apiUrl;       // API 地址
    std::string apiKey;       // API 密钥
    std::string modelName;    // 实际模型名，如 "qwen-plus"
};

// ---------------------------------------------------------------------------
// 调用结果
// ---------------------------------------------------------------------------

struct GatewayResult {
    std::string body;          // 响应 JSON 字符串
    std::string backendId;     // 实际处理请求的后端 ID
    std::uint64_t circuitPermit = 0;
    bool        fallbackUsed = false;
    long long   latencyMs = 0;
};

// ---------------------------------------------------------------------------
// 计时工具
// ---------------------------------------------------------------------------

inline long long nowMs() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
}
