#pragma once

#include <stdexcept>
#include <string>
#include <chrono>

// ---------------------------------------------------------------------------
// 异常体系 — 全部继承 std::runtime_error，兼容现有 catch(const std::exception&)
// ---------------------------------------------------------------------------

class GatewayException : public std::runtime_error {
public:
    explicit GatewayException(const std::string& msg) : std::runtime_error(msg) {}
};

class RateLimitException : public GatewayException {
public:
    explicit RateLimitException(const std::string& msg) : GatewayException(msg) {}
};

class CircuitOpenException : public GatewayException {
public:
    explicit CircuitOpenException(const std::string& msg) : GatewayException(msg) {}
};

class TimeoutException : public GatewayException {
public:
    explicit TimeoutException(const std::string& msg) : GatewayException(msg) {}
};

class BackendException : public GatewayException {
public:
    explicit BackendException(const std::string& msg) : GatewayException(msg) {}
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
