#include "../../include/LLMGateway/LLMGateway.h"
#include <curl/curl.h>
#include <iostream>

// ============================================================================
// curl 写回调与本文件私有辅助函数
// ============================================================================

namespace {
    size_t curlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        size_t total = size * nmemb;
        static_cast<std::string*>(userp)->append(static_cast<char*>(contents), total);
        return total;
    }

    // 从后端返回体提取可读错误信息。
    // 兼容 OpenAI / DashScope 常见错误格式：
    //   {"error":{"message":...,"code":...}}
    //   {"error":"..."}
    //   {"message":"..."}
    // 如果 body 不是 JSON，则返回截断后的原始响应，避免日志/异常信息过长。
    std::string extractBackendError(const std::string& body) {
        try {
            auto j = json::parse(body);
            if (j.contains("error")) {
                const auto& err = j["error"];
                if (err.is_object()) {
                    std::string msg = err.value("message", std::string{});
                    std::string code = err.value("code", std::string{});
                    if (!code.empty()) msg += " (code=" + code + ")";
                    if (!msg.empty()) return msg;
                    return err.dump();
                }
                if (err.is_string()) return err.get<std::string>();
            }
            if (j.contains("message") && j["message"].is_string()) {
                return j["message"].get<std::string>();
            }
        } catch (...) {
            // 非 JSON：返回截断的原始片段
        }
        return body.size() > 200 ? body.substr(0, 200) + "..." : body;
    }

    // 流式回调上下文。curl 的写回调可能拿到半行数据，所以需要 buffer 缓存。
    // raw 用于在非 SSE 错误响应时提取后端错误；正常流式数据仍然会保留在 raw 中。
    struct StreamContext {
        ChunkCallback onChunk;
        std::string   buffer;          // 尚未处理完的半行/多行数据
        std::string   raw;             // 累积全部原始字节，用于错误检测
        bool          sawData = false; // 是否出现过 SSE "data:" 行
    };

    size_t streamingWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        auto* ctx = static_cast<StreamContext*>(userp);
        size_t total = size * nmemb;

        ctx->raw.append(static_cast<char*>(contents), total);
        ctx->buffer.append(static_cast<char*>(contents), total);

        // 按行拆分，处理完整的 SSE 行。这里不按 "\n\n" 事件块解析，
        // 是因为当前只关心 data: 行，event/id/retry 等字段会被忽略。
        size_t pos;
        while ((pos = ctx->buffer.find('\n')) != std::string::npos) {
            std::string line = ctx->buffer.substr(0, pos);
            ctx->buffer.erase(0, pos + 1);

            // 去掉行尾的 \r
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            // 跳过空行
            if (line.empty()) continue;

            // 只处理 "data:" 前缀。兼容 "data: {...}" 和 "data:{...}"。
            if (line.rfind("data:", 0) == 0) {
                std::string data = line.substr(5);
                if (!data.empty() && data.front() == ' ') {
                    data.erase(0, 1);
                }

                // [DONE] 只表示后端流结束，不再向上层回调。
                if (data == "[DONE]") continue;

                ctx->sawData = true;
                ctx->onChunk(data);
            }
        }
        return total;
    }
}

// ============================================================================
// 单例
// ============================================================================

LLMGateway& LLMGateway::instance() {
    static LLMGateway gw;
    return gw;
}

// ============================================================================
// 初始化
//
// init 只做出口治理组件的初始化：限流桶参数、熔断器参数和 fallback 路由配置。
// 模型列表、MCP/RAG 策略选择、请求体格式构造都在 AIHelper/AIStrategy 层完成。
// ============================================================================

void LLMGateway::init(const GatewayConfig& config) {
    config_     = config;
    initialized_ = true;

    if (!config_.enabled) {
        std::cout << "[LLMGateway] Disabled — pass-through mode" << std::endl;
        return;
    }

    // 配置显式出现在 gateway_config.rate_limit.per_backend 中的后端限流桶。
    // 没有配置限流的后端在 checkBackendAllowed 中会直接放行。
    for (auto& [id, entry] : config_.backendRateLimits) {
        rateLimiter_.configureBackend(id, entry.requestsPerSecond, entry.burstSize);
    }
    rateLimiter_.configureUserLimit(config_.userRps, config_.userBurst);

    // 预创建 gateway_config.backends 中的熔断器。
    // 主模型 modelId 即使不在 backends 里，也会在 getCircuitBreaker 中按需创建。
    for (auto& [id, be] : config_.backends) {
        auto& cb = getCircuitBreaker(id);
        cb.setBackendId(id);
        cb.configure(config_.cbFailureThreshold,
                     config_.cbFailureResetTimeoutMs,
                     config_.cbRecoveryTimeoutMs,
                     config_.cbHalfOpenMaxCalls,
                     config_.cbSuccessThreshold);
    }

    std::cout << "[LLMGateway] Initialized: "
              << config_.backends.size() << " backends, "
              << config_.routing.size() << " routing entries" << std::endl;
}

// ============================================================================
// 获取或创建熔断器
//
// 每个 backendId/modelId 对应一个独立熔断器。fallback 后端使用 gateway_config.backends
// 中的 id；主后端使用上层传入的 modelId。
// ============================================================================

CircuitBreaker& LLMGateway::getCircuitBreaker(const std::string& backendId) {
    std::lock_guard<std::mutex> lock(cbMutex_);
    // try_emplace 在 map 节点内原地构造，避免移动 mutex
    auto [it, inserted] = circuitBreakers_.try_emplace(backendId);
    if (inserted) {
        it->second.setBackendId(backendId);
        it->second.configure(config_.cbFailureThreshold,
                             config_.cbFailureResetTimeoutMs,
                             config_.cbRecoveryTimeoutMs,
                             config_.cbHalfOpenMaxCalls,
                             config_.cbSuccessThreshold);
    }
    return it->second;
}

// ============================================================================
// executeHttp — 单次非流式 HTTP POST
//
// 该函数只负责真实 HTTP 交互：设置 header、发送 requestBody、收集完整响应体、
// 根据 curl 错误码和 HTTP 状态码抛出异常。它不做限流、熔断和 fallback，
// 这些由 call()/tryBackend() 负责。
// ============================================================================

std::string LLMGateway::executeHttp(const std::string& url,
                                    const std::string& apiKey,
                                    const std::string& requestBody) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw BackendException("curl_easy_init() failed");
    }

    std::string readBuffer;
    struct curl_slist* headers = nullptr;

    // 连接超时控制 TCP/TLS 建连阶段；总请求超时覆盖整个请求生命周期。
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, config_.connectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, config_.requestTimeoutMs);

    // 设置 URL
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    // 当前后端统一按 Bearer Token + JSON 请求体发送。
    std::string authHeader = "Authorization: Bearer " + apiKey;
    headers = curl_slist_append(headers, authHeader.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // requestBody 已经由上层 json.dump() 生成；这里不再修改内容。
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)requestBody.size());

    // 非流式接口把完整响应体收集到 readBuffer，返回后再解析 JSON。
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

    // curl_easy_perform 会阻塞当前业务线程，直到请求完成、超时或失败。
    CURLcode res = curl_easy_perform(curl);

    // HTTP 状态码必须在 curl_easy_cleanup 之前读取。
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    // 先释放 curl 资源，再把错误转换成网关异常。
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        const char* errStr = curl_easy_strerror(res);
        if (res == CURLE_OPERATION_TIMEDOUT) {
            throw TimeoutException(
                "Request to " + url + " timed out: " + errStr);
        }
        throw BackendException(
            "curl_easy_perform() failed: " + std::string(errStr));
    }

    // 传输层成功（curl OK）不等于业务成功：HTTP 4xx/5xx 是后端业务错误（欠费/鉴权/限流等）
    if (httpCode >= 400) {
        throw BackendException(
            "Backend HTTP " + std::to_string(httpCode) + ": " + extractBackendError(readBuffer));
    }

    return readBuffer;
}
// ============================================================================
// executeHttpStreaming — 单次流式 HTTP POST
//
// 该函数只负责把后端 SSE 响应拆出 data: 行，并把 data 内容交给 onChunk。
// 它不解析模型业务 JSON，也不会拼接完整回答；上层 AIHelper/AIStrategy 负责解析
// delta.content、累积 full result 和落库。
// ============================================================================

void LLMGateway::executeHttpStreaming(const std::string& url,
                                      const std::string& apiKey,
                                      const std::string& requestBody,
                                      ChunkCallback onChunk,
                                      const std::vector<std::pair<std::string, std::string>>& extraHeaders)
{
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw BackendException("curl_easy_init() failed");
    }

    StreamContext ctx;
    ctx.onChunk = std::move(onChunk);

    struct curl_slist* headers = nullptr;

    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, config_.connectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,      config_.requestTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_URL,              url.c_str());

    std::string authHeader = "Authorization: Bearer " + apiKey;
    headers = curl_slist_append(headers, authHeader.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    // 追加策略指定的额外 header（如 RAG 的 X-DashScope-SSE）
    for (const auto& h : extraHeaders) {
        std::string headerLine = h.first + ": " + h.second;
        headers = curl_slist_append(headers, headerLine.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    requestBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)requestBody.size());

    // 关键：使用流式写回调。后端每推送一批字节，curl 都可能立刻进入回调。
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, streamingWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &ctx);

    CURLcode res = curl_easy_perform(curl);

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        const char* errStr = curl_easy_strerror(res);
        if (res == CURLE_OPERATION_TIMEDOUT) {
            throw TimeoutException(
                "Streaming request to " + url + " timed out: " + errStr);
        }
        throw BackendException(
            "curl_easy_perform() failed: " + std::string(errStr));
    }

    // HTTP 4xx/5xx：鉴权、欠费、参数错误、后端限流等业务失败。
    // 这些失败通常不是正常 token 流，需要抛出给上层转成前端 SSE error。
    if (httpCode >= 400) {
        throw BackendException(
            "Backend HTTP " + std::to_string(httpCode) + ": " + extractBackendError(ctx.raw));
    }

    // 状态码 200 但整个响应没有任何 data: 行：通常说明后端返回了普通 JSON 错误，
    // 或者上层没有正确打开流式参数。这里主动报错，避免前端只看到空白回答。
    if (!ctx.sawData && !ctx.raw.empty()) {
        throw BackendException(
            "Backend returned no stream data: " + extractBackendError(ctx.raw));
    }
}

                                            
// ============================================================================
// tryBackend — 对单个非流式后端尝试请求
//
// call() 用它统一处理 primary 和 fallback 后端。该函数吞掉单个后端的
// GatewayException 并返回 false，让 call() 有机会继续尝试下一个 fallback。
// 用户级限流不在这里做，避免一次请求因多个 fallback 重复消耗用户令牌。
// ============================================================================

bool LLMGateway::tryBackend(const BackendConfig& backend,
                            const json&          payload,
                            int                  userId,
                            GatewayResult&       result) {
    // 1. 后端级限流：每尝试一个实际后端，都要消耗该后端自己的令牌。
    try {
        rateLimiter_.checkBackendAllowed(backend.id);
    } catch (const RateLimitException& e) {
        std::cout << "[LLMGateway] Backend " << backend.id
                  << " rate limited: " << e.what() << std::endl;
        return false;
    }

    // 2. 熔断器检查：OPEN 状态直接跳过该后端，HALF_OPEN/CLOSED 由熔断器决定。
    CircuitBreaker& cb = getCircuitBreaker(backend.id);
    if (!cb.allowRequest()) {
        std::cout << "[LLMGateway] Backend " << backend.id
                  << " circuit not allowing request" << std::endl;
        return false;
    }

    // 3. 准备请求体。fallback 后端可能和主后端使用同一种 OpenAI 兼容格式，
    //    但实际模型名不同，所以只在 payload 原本有 "model" 字段时替换它。
    //    RAG 这类没有 "model" 字段的请求体不会被这里改写。
    json reqBody = payload;
    if (!backend.modelName.empty() && reqBody.contains("model")) {
        reqBody["model"] = backend.modelName;
    }

    std::string requestStr = reqBody.dump();

    // 4. 执行 HTTP 请求。这里先只拿到 body；最终是否算成功，要等 call()
    //    完成 JSON 解析后再上报熔断器，避免 HTTP 2xx 但业务响应不可解析被误记成功。
    long long startMs = nowMs();
    try {
        std::string responseBody = executeHttp(backend.apiUrl, backend.apiKey, requestStr);

        long long elapsed = nowMs() - startMs;

        result.body         = std::move(responseBody);
        result.backendId    = backend.id;
        result.latencyMs    = elapsed;

        return true;

    } catch (const GatewayException&) {
        // 单个后端失败时不向外抛，交给 call() 决定是否 fallback。
        cb.reportFailure();
        std::cout << "[LLMGateway] " << backend.id
                  << " failed (gateway exception)" << std::endl;
        return false;
    }
}

// ============================================================================
// call — 非流式网关入口
//
// 完整链路：
//   直通模式：executeHttp(primary) → parse JSON
//   网关模式：用户级限流 → try primary → 按 routing 尝试 fallback → parse JSON
//
// 注意：这里返回的是原始响应 JSON，具体从 choices/output 中取文本由 AIStrategy 完成。
// ============================================================================

json LLMGateway::call(const std::string& modelId,
                      const json&        payload,
                      const std::string& primaryUrl,
                      const std::string& primaryKey,
                      int                userId) {
    // 未初始化或禁用：直通模式。仍然使用 executeHttp 的超时和 HTTP 错误检查，
    // 但不做用户/后端限流、熔断和 fallback。
    if (!initialized_ || !config_.enabled) {
        std::string body = executeHttp(primaryUrl, primaryKey, payload.dump());
        try {
            return json::parse(body);
        } catch (...) {
            throw BackendException("Failed to parse LLM response");
        }
    }

    // 1. 每用户限流。一次业务请求只检查一次，不随 fallback 次数重复扣令牌。
    rateLimiter_.checkUserAllowed(userId);

    // 2. 构建主后端信息。主后端来自 AIStrategy，因此不要求出现在 gateway_config.backends。
    BackendConfig primary;
    primary.id        = modelId;
    primary.apiUrl    = primaryUrl;
    primary.apiKey    = primaryKey;
    primary.modelName = "";  // 主后端不修改 payload，策略已经放入正确模型名/请求格式。

    // 3. 尝试主后端。主后端成功返回 HTTP body 后，这里再解析成 JSON。
    GatewayResult result;
    if (tryBackend(primary, payload, userId, result)) {
        try {
            json parsed = json::parse(result.body);
            getCircuitBreaker(modelId).reportSuccess();
            std::cout << "[LLMGateway] " << result.backendId
                      << " OK (" << result.latencyMs << "ms)" << std::endl;
            return parsed;
        } catch (...) {
            // HTTP 成功但 JSON 解析失败，也视为该后端失败，继续 fallback。
            getCircuitBreaker(modelId).reportFailure();
        }
    }

    // 4. 尝试 fallback 链。fallback 后端必须定义在 gateway_config.backends 中。
    auto routeIt = config_.routing.find(modelId);
    if (routeIt != config_.routing.end()) {
        for (const auto& fbId : routeIt->second) {
            auto beIt = config_.backends.find(fbId);
            if (beIt == config_.backends.end()) {
                std::cerr << "[LLMGateway] Fallback '" << fbId
                          << "' not found in backends config" << std::endl;
                continue;
            }

            std::cout << "[LLMGateway] Falling back: " << modelId
                      << " -> " << fbId << std::endl;

            if (tryBackend(beIt->second, payload, userId, result)) {
                try {
                    json parsed = json::parse(result.body);
                    getCircuitBreaker(fbId).reportSuccess();
                    std::cout << "[LLMGateway] " << result.backendId
                              << " OK (" << result.latencyMs << "ms)" << std::endl;
                    return parsed;
                } catch (...) {
                    getCircuitBreaker(fbId).reportFailure();
                    // fallback 返回体不是合法 JSON，继续下一个 fallback。
                }
            }
        }
    }

    // 5. 所有可用后端都失败/限流/熔断，交给上层 handler 转成前端错误。
    throw BackendException(
        "All backends exhausted for '" + modelId +
        "': circuit open, rate limited, or all requests failed");
}

// ============================================================================
// callStreaming — 流式网关入口
//
// 完整链路：
//   直通模式：executeHttpStreaming(primary)
//   网关模式：用户级限流 → 后端级限流 → 熔断检查 → executeHttpStreaming(primary)
//
// 当前不支持 fallback：因为一旦 onChunk 已经把部分 token 推给前端，换后端续写
// 会造成内容不可回滚。调用方应把 payload["stream"] = true 等流式字段提前放好。
// ============================================================================

void LLMGateway::callStreaming(const std::string& modelId,
                                const json&        payload,
                                const std::string& primaryUrl,
                                const std::string& primaryKey,
                                int                userId,
                                ChunkCallback      onChunk,
                                const std::vector<std::pair<std::string, std::string>>& extraHeaders) {
    // 未初始化或禁用：直通流式。仍做 curl 超时/HTTP 错误检查，但不做治理。
    if (!initialized_ || !config_.enabled) {
        executeHttpStreaming(primaryUrl, primaryKey, payload.dump(), onChunk, extraHeaders);
        return;
    }

    // 每用户限流：一次流式请求只检查一次。
    rateLimiter_.checkUserAllowed(userId);

    // 后端级限流：流式无 fallback，所以只检查主后端 modelId。
    rateLimiter_.checkBackendAllowed(modelId);

    // 熔断器检查：OPEN 时直接拒绝，不发起 HTTP。
    CircuitBreaker& cb = getCircuitBreaker(modelId);
    if (!cb.allowRequest()) {
        throw CircuitOpenException("Circuit breaker not allowing request for " + modelId);
    }

    long long startMs = nowMs();
    try {
        executeHttpStreaming(primaryUrl, primaryKey, payload.dump(), onChunk, extraHeaders);
        long long elapsed = nowMs() - startMs;
        cb.reportSuccess();
        std::cout << "[LLMGateway] Streaming " << modelId
                  << " OK (" << elapsed << "ms)" << std::endl;
    } catch (const GatewayException&) {
        cb.reportFailure();
        throw;  // 流式不走 fallback，上层 handler 会把异常转成 SSE error。
    }
}
