#include"../include/AIUtil/AIHelper.h"
#include"../include/AIUtil/MQManager.h"
#include"../include/LLMGateway/LLMGateway.h"
#include <stdexcept>
#include<chrono>

// 构造函数
AIHelper::AIHelper() {
    // 使用配置文件中的默认模型
    auto& factory = StrategyFactory::instance();
    strategy = factory.create(factory.getDefaultModel());
}

void AIHelper::setStrategy(std::shared_ptr<AIStrategy> strat) {
    strategy = strat;
}


// 设置默认模型
//void AIHelper::setModel(const std::string& modelName) {
  //  model_ = modelName;
//}

// 添加一条用户消息
void AIHelper::addMessage(int userId,const std::string& userName, bool is_user,const std::string& userInput, std::string sessionId) {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    // mutex_ 由调用方（chat）持有，此处不再重复加锁
    messages.push_back({ userInput,ms });
    //消息队列异步入库
    pushMessageToMysql(userId, userName, is_user, userInput, ms, sessionId);
}
// 回复消息
void AIHelper::restoreMessage(const std::string& userInput,long long ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    messages.push_back({ userInput,ms });
}

// 统一设置模型策略并打印日志
void AIHelper::prepareStrategy(const std::string& modelType) {
    setStrategy(StrategyFactory::instance().create(modelType));
    std::cout << "[AIHelper] modelType=" << modelType
              << " -> strategy: model=" << strategy->getModel()
              << " url=" << strategy->getApiUrl() << std::endl;
}

// 工具路由：构造路由 prompt，执行非流式 LLM 调用，解析工具决策
AIToolCall AIHelper::routeToolCall(int userId,
                                   const std::string& userQuestion,
                                   const std::string& modelType,
                                   AIConfig& config) {
    std::string routePrompt = config.buildPrompt(userQuestion);
    std::cout << "[AIHelper::routeToolCall] routePrompt preview="
              << routePrompt.substr(0, 120) << std::endl;

    messages.push_back({routePrompt, 0});

    try {
        json firstReq = buildRequestWithContext();
        json firstResp = executeCurl(firstReq, userId, modelType);
        std::string routeResult = strategy->parseResponse(firstResp);
        messages.pop_back();

        std::cout << "[AIHelper::routeToolCall] routeResult=" << routeResult << std::endl;
        return config.parseAIResponse(routeResult);
    } catch (...) {
        messages.pop_back();
        throw;
    }
}

// 流式最终回答：基于当前 messages 构造请求，调用网关流式接口
void AIHelper::completeStreaming(int userId,
                                 const std::string& modelType,
                                 ChunkCallback onChunk,
                                 std::string& fullResult) {
    json payload = buildRequestWithContext();
    strategy->enableStreaming(payload);

    std::cout << "[AIHelper::completeStreaming] url=" << strategy->getApiUrl() << std::endl;

    auto extraHeaders = strategy->getExtraHeaders();
    LLMGateway::instance().callStreaming(
        modelType, payload,
        strategy->getApiUrl(), strategy->getApiKey(),
        userId,
        [&](const std::string& chunk) {
            std::string parsedChunk = strategy->parseStreamChunk(chunk);
            if (!parsedChunk.empty()) {
                fullResult += parsedChunk;
                onChunk(parsedChunk);
            }
        },
        extraHeaders
    );
}

// 统一聊天入口：所有响应均通过 SSE 流式返回
void AIHelper::chat(int userId,
                    std::string userName,
                    std::string sessionId,
                    std::string userQuestion,
                    std::string modelType,
                    bool enableTools,
                    ChunkCallback onChunk) {
    std::lock_guard<std::mutex> lock(mutex_);
    prepareStrategy(modelType);

    // ── 分支 1：不需要工具 → 直接流式回答 ──
    if (!enableTools || !strategy->supportTools()) {
        addMessage(userId, userName, true, userQuestion, sessionId);
        std::string fullResult;
        completeStreaming(userId, modelType, onChunk, fullResult);
        addMessage(userId, userName, false, fullResult, sessionId);
        return;
    }

    // ── 分支 2：需要工具 → 路由 → 工具执行 → 流式最终回答 ──
    AIConfig config;
    if (!config.loadFromFile("../AIApps/ChatServer/resource/config.json")) {
        std::cerr << "[AIHelper] Config load failed, falling back to direct streaming" << std::endl;
        addMessage(userId, userName, true, userQuestion, sessionId);
        std::string fullResult;
        completeStreaming(userId, modelType, onChunk, fullResult);
        addMessage(userId, userName, false, fullResult, sessionId);
        return;
    }

    // 第一次调用：工具路由（非流式）
    AIToolCall call;
    try {
        call = routeToolCall(userId, userQuestion, modelType, config);
    } catch (const std::exception& e) {
        std::string err = "[路由调用失败] " + std::string(e.what());
        std::cerr << "[AIHelper] " << err << std::endl;
        addMessage(userId, userName, true, userQuestion, sessionId);
        addMessage(userId, userName, false, err, sessionId);
        onChunk(err);
        return;
    }

    // 执行工具调用（如果需要）
    json toolResult;
    if (call.isToolCall) {
        AIToolRegistry registry;
        try {
            toolResult = registry.invoke(call.toolName, call.args);
            std::cout << "[AIHelper] Tool call success: " << call.toolName << std::endl;
        } catch (const std::exception& e) {
            std::string err = "[工具调用失败] " + std::string(e.what());
            std::cerr << "[AIHelper] " << err << std::endl;
            addMessage(userId, userName, true, userQuestion, sessionId);
            addMessage(userId, userName, false, err, sessionId);
            onChunk(err);
            return;
        }
    }

    // 构造最终回答 prompt
    std::string finalPrompt = call.isToolCall
        ? config.buildToolResultPrompt(userQuestion, call.toolName, call.args, toolResult)
        : userQuestion;

    std::cout << "[AIHelper] finalPrompt preview="
              << finalPrompt.substr(0, 120) << std::endl;

    // 临时压入 messages 用于 LLM 上下文；回答落库前弹出
    messages.push_back({finalPrompt, 0});

    // 第二次调用：流式最终回答
    std::string fullResult;
    try {
        completeStreaming(userId, modelType, onChunk, fullResult);
    } catch (...) {
        messages.pop_back();
        throw;
    }
    messages.pop_back();

    // 落库：只保存原始用户问题和最终 AI 答案
    addMessage(userId, userName, true, userQuestion, sessionId);
    addMessage(userId, userName, false, fullResult, sessionId);
}
// 发送自定义请求体
json AIHelper::request(const json& payload) {
    return executeCurl(payload, 0, strategy->getModel());
}

std::vector<std::pair<std::string, long long>> AIHelper::GetMessages() {
    std::lock_guard<std::mutex> lock(mutex_);
    return this->messages;
}


// 内部方法：通过网关执行 HTTP 请求，返回原始 JSON
json AIHelper::executeCurl(const json& payload, int userId, const std::string& modelType) {
    std::cout << "[AIHelper] request via gateway, model=" << modelType
              << " url=" << strategy->getApiUrl() << std::endl;

    return LLMGateway::instance().call(
        modelType,
        payload,
        strategy->getApiUrl(),
        strategy->getApiKey(),
        userId);
}

// curl 回调函数，把返回的数据写到 string buffer
size_t AIHelper::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    std::string* buffer = static_cast<std::string*>(userp);
    buffer->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

std::string AIHelper::escapeString(const std::string& input) {
    std::string output;
    output.reserve(input.size() * 2);
    for (char c : input) {
        switch (c) {
            case '\\': output += "\\\\"; break;
            case '\'': output += "\\\'"; break;
            case '\"': output += "\\\""; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:   output += c; break;
        }
    }
    return output;
}

// 辅助方法：上下文窗口裁剪
json AIHelper::buildRequestWithContext() {
    int maxContext = strategy->getMaxContext();
    int total = static_cast<int>(messages.size()); // 总消息数量

    // 不需要裁剪上下文
    if(maxContext <= 0 || total <= maxContext * 2 + 2){
        return strategy->buildRequest(messages);
    }

    // 裁剪上下文：保留最新maxContext轮对话
    int recent = maxContext * 2; // 每轮对话包含用户和AI两条消息
    int omitted = (total - recent - 2) / 2; // 被裁剪掉的轮数

    // 构建送入模型的上下文窗口
    std::vector<std::pair<std::string, long long>> context;
    context.push_back(messages[0]); // 用户最早的提问
    context.push_back(messages[1]); // ai最早的回答
    for(int i = total - recent; i < total; ++i){
        context.push_back(messages[i]);
    }

    json payload = strategy->buildRequest(context);
    // 告知模型有消息被裁剪
    json sysMsg;
    sysMsg["role"] = "system";
    sysMsg["content"] = "注意：由于上下文长度限制，中间有" + std::to_string(omitted)
                        + "轮对话被裁剪，请根据当前剩余窗口内容回答用户的问题。";

    auto& msgArray = payload["messages"];
    msgArray.insert(msgArray.begin() + 2, sysMsg); // 插入到第一轮对话之后

    return payload;
}

void AIHelper::pushMessageToMysql(int userId, const std::string& userName, bool is_user, const std::string& userInput,long long ms, std::string sessionId) {
    std::string safeUserName = escapeString(userName);
    std::string safeUserInput = escapeString(userInput);

    std::string sql = "INSERT INTO chat_message (id, username, session_id, is_user, content, ts) VALUES ("
        + std::to_string(userId) + ", "
        + "'" + safeUserName + "', "
        + sessionId + ", "
        + std::to_string(is_user ? 1 : 0) + ", "
        + "'" + safeUserInput + "', "
        + std::to_string(ms) + ")";

    //改成消息队列异步执行mysql操作，用于流量削峰与解耦逻辑
    MQManager::instance().publish("sql_queue", sql);
}
