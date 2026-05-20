#pragma once

#include <mutex>
#include <unordered_map>

#include "GatewayTypes.h"
#include "GatewayConfig.h"
#include "RateLimiter.h"
#include "CircuitBreaker.h"
#include "../../../../HttpServer/include/utils/JsonUtil.h"

// ---------------------------------------------------------------------------
// LLMGateway — 网关单例
//   调用链：限流 → 熔断检查 → curl(带超时) → 失败降级 → 重试 fallback
// ---------------------------------------------------------------------------

// 流式回调
using ChunkCallback = std::function<void(const std::string& chunk)>;

class LLMGateway {
public:
    static LLMGateway& instance();

    // 启动时调用一次（主线程，无竞争）
    void init(const GatewayConfig& config);

    // 核心入口：发送 LLM 请求，返回解析后的 JSON 响应
    // modelId:    主后端标识（如 "aliyun-qwen"）
    // payload:    已构建好的请求体（由 AIStrategy::buildRequest 产出）
    // primaryUrl / primaryKey: 来自策略的主后端连接信息
    // userId:     用于每用户限流，0 表示不启用用户级限流
    //
    // 抛出: RateLimitException / CircuitOpenException / TimeoutException / BackendException
    json call(const std::string& modelId,
              const json&        payload,
              const std::string& primaryUrl,
              const std::string& primaryKey,
              int                userId);
    
    // 流式调用
    void callStreaming(const std::string& modelId,
                       const json&        payload,
                       const std::string& primaryUrl,
                       const std::string& primaryKey,
                       int                userId,
                       ChunkCallback      onChunk);

    bool isEnabled() const { return config_.enabled; }

private:
    LLMGateway() = default;
    LLMGateway(const LLMGateway&) = delete;
    LLMGateway& operator=(const LLMGateway&) = delete;

    // 对单个后端执行 HTTP 请求（带超时），返回响应 body 字符串
    // 失败时抛出 BackendException
    std::string executeHttp(const std::string& url,
                            const std::string& apiKey,
                            const std::string& requestBody);
    void executeHttpStreaming(const std::string& url,
                                    const std::string& apiKey,
                                    const std::string& requestBody,
                                    ChunkCallback onChunk);

    // 对单个后端尝试请求：限流 + 熔断 + HTTP
    // 返回 true 表示成功，result 中有响应
    bool tryBackend(const BackendConfig& backend,
                    const json&          payload,
                    int                  userId,
                    GatewayResult&       result);

    // 获取或创建指定后端的熔断器
    CircuitBreaker& getCircuitBreaker(const std::string& backendId);

    GatewayConfig config_;
    bool          initialized_ = false;

    RateLimiter rateLimiter_;

    std::unordered_map<std::string, CircuitBreaker> circuitBreakers_;
    std::mutex cbMutex_;
};
