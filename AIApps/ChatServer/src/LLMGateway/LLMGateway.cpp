#include "../../include/LLMGateway/LLMGateway.h"
#include <curl/curl.h>
#include <iostream>

// ============================================================================
// curl 写回调
// ============================================================================

namespace {
    size_t curlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        size_t total = size * nmemb;
        static_cast<std::string*>(userp)->append(static_cast<char*>(contents), total);
        return total;
    }

    // 流式回调上下文
    struct StreamContext {
        ChunkCallback onChunk;
        std::string   buffer;   // 缓存不完整的行
    };

    size_t streamingWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        auto* ctx = static_cast<StreamContext*>(userp);
        size_t total = size * nmemb;

        ctx->buffer.append(static_cast<char*>(contents), total);

        // 按行拆分，处理完整的 SSE 行
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

            // 检查 "data: " 前缀
            if (line.rfind("data: ", 0) == 0) {
                std::string data = line.substr(6);

                if (data == "[DONE]") continue;

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
// ============================================================================

void LLMGateway::init(const GatewayConfig& config) {
    config_     = config;
    initialized_ = true;

    if (!config_.enabled) {
        std::cout << "[LLMGateway] Disabled — pass-through mode" << std::endl;
        return;
    }

    // 配置限流器
    for (auto& [id, entry] : config_.backendRateLimits) {
        rateLimiter_.configureBackend(id, entry.requestsPerSecond, entry.burstSize);
    }
    rateLimiter_.configureUserLimit(config_.userRps, config_.userBurst);

    // 预热熔断器
    for (auto& [id, be] : config_.backends) {
        auto& cb = getCircuitBreaker(id);
        cb.setBackendId(id);
        cb.configure(config_.cbFailureThreshold,
                     config_.cbRecoveryTimeoutMs,
                     config_.cbHalfOpenMax);
    }

    std::cout << "[LLMGateway] Initialized: "
              << config_.backends.size() << " backends, "
              << config_.routing.size() << " routing entries" << std::endl;
}

// ============================================================================
// 获取或创建熔断器
// ============================================================================

CircuitBreaker& LLMGateway::getCircuitBreaker(const std::string& backendId) {
    std::lock_guard<std::mutex> lock(cbMutex_);
    // try_emplace 在 map 节点内原地构造，避免移动 mutex
    auto [it, inserted] = circuitBreakers_.try_emplace(backendId);
    if (inserted) {
        it->second.setBackendId(backendId);
        it->second.configure(config_.cbFailureThreshold,
                             config_.cbRecoveryTimeoutMs,
                             config_.cbHalfOpenMax);
    }
    return it->second;
}

// ============================================================================
// executeHttp — 带超时的 curl 调用
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

    // 设置超时
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, config_.connectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, config_.requestTimeoutMs);

    // 设置 URL
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    // 设置请求头
    std::string authHeader = "Authorization: Bearer " + apiKey;
    headers = curl_slist_append(headers, authHeader.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // 设置 POST 数据
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)requestBody.size());

    // 设置写回调
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

    // 执行
    CURLcode res = curl_easy_perform(curl);

    // 先清理，再检查错误（防止泄漏）
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

    return readBuffer;
}
// ============================================================================
// executeHttpStreaming — 流式 curl 调用，逐 SSE 行回调
// ============================================================================

void LLMGateway::executeHttpStreaming(const std::string& url,
                                      const std::string& apiKey,
                                      const std::string& requestBody,
                                      ChunkCallback onChunk)
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
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    requestBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)requestBody.size());

    // 关键：使用流式写回调
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, streamingWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &ctx);

    CURLcode res = curl_easy_perform(curl);

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
}

                                            
// ============================================================================
// tryBackend — 对单个后端尝试请求
// ============================================================================

bool LLMGateway::tryBackend(const BackendConfig& backend,
                            const json&          payload,
                            int                  userId,
                            GatewayResult&       result) {
    // 1. 后端级限流
    try {
        rateLimiter_.checkBackendAllowed(backend.id);
    } catch (const RateLimitException& e) {
        std::cout << "[LLMGateway] Backend " << backend.id
                  << " rate limited: " << e.what() << std::endl;
        return false;
    }

    // 2. 熔断器检查
    CircuitBreaker& cb = getCircuitBreaker(backend.id);
    if (!cb.allowRequest()) {
        std::cout << "[LLMGateway] Backend " << backend.id
                  << " circuit OPEN" << std::endl;
        return false;
    }

    // 3. 准备请求体：如果后端指定了 modelName 且 payload 中有 "model" 字段则替换
    json reqBody = payload;
    if (!backend.modelName.empty() && reqBody.contains("model")) {
        reqBody["model"] = backend.modelName;
    }

    std::string requestStr = reqBody.dump();

    // 4. 执行 HTTP 请求
    long long startMs = nowMs();
    try {
        std::string responseBody = executeHttp(backend.apiUrl, backend.apiKey, requestStr);

        long long elapsed = nowMs() - startMs;
        cb.reportSuccess();

        result.body         = std::move(responseBody);
        result.backendId    = backend.id;
        result.latencyMs    = elapsed;

        std::cout << "[LLMGateway] " << backend.id << " OK (" << elapsed << "ms)" << std::endl;
        return true;

    } catch (const GatewayException&) {
        // 不二次包装，直接上报失败
        cb.reportFailure();
        std::cout << "[LLMGateway] " << backend.id
                  << " failed (gateway exception)" << std::endl;
        return false;
    }
}

// ============================================================================
// call — 网关入口
// ============================================================================

json LLMGateway::call(const std::string& modelId,
                      const json&        payload,
                      const std::string& primaryUrl,
                      const std::string& primaryKey,
                      int                userId) {
    // 未初始化或禁用：直通模式
    if (!initialized_ || !config_.enabled) {
        std::string body = executeHttp(primaryUrl, primaryKey, payload.dump());
        try {
            return json::parse(body);
        } catch (...) {
            throw BackendException("Failed to parse LLM response");
        }
    }

    // 1. 每用户限流（只检查一次）
    rateLimiter_.checkUserAllowed(userId);

    // 2. 构建主后端信息
    BackendConfig primary;
    primary.id        = modelId;
    primary.apiUrl    = primaryUrl;
    primary.apiKey    = primaryKey;
    primary.modelName = "";  // 主后端不修改 payload（策略已设置好）

    // 3. 尝试主后端
    GatewayResult result;
    if (tryBackend(primary, payload, userId, result)) {
        try {
            return json::parse(result.body);
        } catch (...) {
            // JSON 解析失败也算后端失败，继续尝试 fallback
            getCircuitBreaker(modelId).reportFailure();
        }
    }

    // 4. 尝试 fallback 链
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
                    return json::parse(result.body);
                } catch (...) {
                    getCircuitBreaker(fbId).reportFailure();
                    // 继续下一个 fallback
                }
            }
        }
    }

    // 5. 所有后端耗尽
    throw BackendException(
        "All backends exhausted for '" + modelId +
        "': circuit open, rate limited, or all requests failed");
}

// ============================================================================
// callStreaming — 流式网关入口（限流 + 熔断 + 流式 HTTP，无 fallback）
// ============================================================================

void LLMGateway::callStreaming(const std::string& modelId,
                                const json&        payload,
                                const std::string& primaryUrl,
                                const std::string& primaryKey,
                                int                userId,
                                ChunkCallback      onChunk) {
    // 未初始化或禁用：直通流式
    if (!initialized_ || !config_.enabled) {
        executeHttpStreaming(primaryUrl, primaryKey, payload.dump(), onChunk);
        return;
    }

    // 每用户限流
    rateLimiter_.checkUserAllowed(userId);

    // 后端级限流
    rateLimiter_.checkBackendAllowed(modelId);

    // 熔断器检查
    CircuitBreaker& cb = getCircuitBreaker(modelId);
    if (!cb.allowRequest()) {
        throw CircuitOpenException("Circuit breaker open for " + modelId);
    }

    long long startMs = nowMs();
    try {
        executeHttpStreaming(primaryUrl, primaryKey, payload.dump(), onChunk);
        long long elapsed = nowMs() - startMs;
        cb.reportSuccess();
        std::cout << "[LLMGateway] Streaming " << modelId
                  << " OK (" << elapsed << "ms)" << std::endl;
    } catch (const GatewayException&) {
        cb.reportFailure();
        throw;  // 流式不走 fallback
    }
}
