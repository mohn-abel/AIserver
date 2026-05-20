#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include "GatewayTypes.h"
#include "../../../../HttpServer/include/utils/JsonUtil.h"

// ---------------------------------------------------------------------------
// 网关全量配置
// ---------------------------------------------------------------------------

struct GatewayConfig {
    bool enabled = true;

    // 后端定义：backendId → BackendConfig
    std::unordered_map<std::string, BackendConfig> backends;

    // 每后端限流：backendId → {rps, burst}
    struct RateLimitEntry {
        double requestsPerSecond = 10.0;
        double burstSize         = 5.0;
    };
    std::unordered_map<std::string, RateLimitEntry> backendRateLimits;
    double userRps   = 3.0;
    double userBurst = 2.0;

    // 熔断器参数
    int       cbFailureThreshold   = 5;
    long long cbRecoveryTimeoutMs  = 30000;
    int       cbHalfOpenMax        = 3;

    // 超时参数
    long long connectTimeoutMs = 5000;
    long long requestTimeoutMs = 60000;

    // 路由/降级：backendId → fallback 链（按优先级排列）
    std::unordered_map<std::string, std::vector<std::string>> routing;

    // 从 JSON 文件加载，失败时返回 false
    bool loadFromFile(const std::string& path);

private:
    void validate() const;
};
