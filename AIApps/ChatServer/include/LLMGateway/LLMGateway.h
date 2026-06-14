#pragma once

#include <mutex>
#include <unordered_map>

#include "GatewayTypes.h"
#include "GatewayConfig.h"
#include "RateLimiter.h"
#include "CircuitBreaker.h"
#include "../../../../HttpServer/include/utils/JsonUtil.h"

// ---------------------------------------------------------------------------
// LLMGateway — LLM 出口网关单例
//
// 这个类只负责“把已经构造好的请求体发到模型后端”，并在出口处统一做：
//   1. 用户级限流
//   2. 后端级限流
//   3. 熔断检查/状态上报
//   4. curl HTTP 调用和超时控制
//   5. 非流式请求的 fallback 降级
//
// 注意：
//   - MCP/RAG 的业务编排不在这里，网关不理解“是否要调工具”。
//   - payload 由上层 AIStrategy/AIHelper 构造，网关只在 fallback 后端需要时替换
//     OpenAI 兼容请求体里的 "model" 字段。
//   - 流式请求当前不走 fallback，因为部分内容可能已经发送给前端，失败后无法安全回滚。
// ---------------------------------------------------------------------------

// 流式回调。
// executeHttpStreaming/callStreaming 传给回调的是后端 SSE 中 data: 后面的原始字符串，
// 通常是一段 JSON；具体如何提取 delta.content 由 AIStrategy::parseStreamChunk 负责。
using ChunkCallback = std::function<void(const std::string& chunk)>;

class LLMGateway {
public:
    static LLMGateway& instance();

    // 启动时调用一次，写入网关配置并初始化限流器/熔断器。
    // 如果 config.enabled=false，后续请求会进入直通模式：仍然用 curl 发送，
    // 但不做限流、熔断和 fallback。
    void init(const GatewayConfig& config);

    // 非流式入口：发送一次 LLM 请求，返回解析后的 JSON 响应。
    //
    // modelId:
    //   上层选择的模型/主后端标识，例如 "aliyun-qwen"。
    // payload:
    //   已构造好的请求体。网关不会补业务字段；如果请求需要 stream/tools/input 等字段，
    //   必须由上层提前放入。
    // primaryUrl / primaryKey:
    //   主后端连接信息，来自当前 AIStrategy。
    // userId:
    //   用于用户级限流；userId<=0 时 RateLimiter 会跳过用户级限流。
    //
    // 行为：
    //   - 网关未初始化或被禁用时：直连 primaryUrl，不做治理和 fallback。
    //   - 网关启用时：先尝试 primary，再按 GatewayConfig::routing 尝试 fallback 链。
    //
    // 抛出: RateLimitException / CircuitOpenException / TimeoutException / BackendException
    json call(const std::string& modelId,
              const json&        payload,
              const std::string& primaryUrl,
              const std::string& primaryKey,
              int                userId);
    
    // 流式入口：发送一次流式 LLM 请求，逐个 SSE data 块回调 onChunk。
    //
    // 调用方必须提前把开启流式所需字段放进 payload，例如 OpenAI 兼容接口的
    // payload["stream"] = true。网关只转发请求体，不会自动打开流式开关。
    //
    // 行为：
    //   - 网关未初始化或被禁用时：直连 primaryUrl。
    //   - 网关启用时：做用户级限流、后端级限流、熔断检查。
    //   - 当前没有 fallback。原因是流式响应一旦向前端吐出部分 token，
    //     失败后切换后端会造成内容重复、语义断裂或无法落库。
    void callStreaming(const std::string& modelId,
                       const json&        payload,
                       const std::string& primaryUrl,
                       const std::string& primaryKey,
                       int                userId,
                       ChunkCallback      onChunk,
                       const std::vector<std::pair<std::string, std::string>>& extraHeaders = {});

    bool isEnabled() const { return config_.enabled; }

private:
    LLMGateway() = default;
    LLMGateway(const LLMGateway&) = delete;
    LLMGateway& operator=(const LLMGateway&) = delete;

    // 对单个后端执行普通 HTTP POST（带连接/总请求超时），返回响应 body 字符串。
    // curl 失败、HTTP 4xx/5xx 或业务错误会抛 GatewayException 的子类。
    std::string executeHttp(const std::string& url,
                            const std::string& apiKey,
                            const std::string& requestBody);

    // 对单个后端执行流式 HTTP POST。
    // 只解析 SSE 行边界和 data: 前缀，不解析业务 JSON；每个 data 块交给 onChunk。
    // curl 失败、HTTP 4xx/5xx 或“返回体不是 SSE data 流”会抛 GatewayException 的子类。
    void executeHttpStreaming(const std::string& url,
                                    const std::string& apiKey,
                                    const std::string& requestBody,
                                    ChunkCallback onChunk,
                                    const std::vector<std::pair<std::string, std::string>>& extraHeaders = {});

    // 对单个非流式后端尝试请求：后端级限流 → 熔断检查 → HTTP。
    // 返回 true 表示拿到 HTTP 2xx 响应体；返回 false 表示该后端不可用或请求失败，
    // call() 可以继续尝试下一个 fallback。
    bool tryBackend(const BackendConfig& backend,
                    const json&          payload,
                    int                  userId,
                    GatewayResult&       result);

    // 获取或创建指定后端的熔断器。
    // primary modelId 不一定出现在 gateway_config.backends 中，所以这里允许按需创建。
    CircuitBreaker& getCircuitBreaker(const std::string& backendId);

    GatewayConfig config_;
    bool          initialized_ = false;

    RateLimiter rateLimiter_;

    std::unordered_map<std::string, CircuitBreaker> circuitBreakers_;
    std::mutex cbMutex_;
};
