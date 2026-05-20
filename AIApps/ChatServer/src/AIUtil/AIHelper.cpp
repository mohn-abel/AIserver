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

// chat内部方法，调用前需持有mutex_
std::string AIHelper::chatInternal(int userId,std::string userName, std::string sessionId, std::string userQuestion, std::string modelType) {
    //设置策略
    setStrategy(StrategyFactory::instance().create(modelType));
    std::cout << "[AIHelper::chat] received modelType=" << modelType
              << " -> strategy: model=" << strategy->getModel()
              << " url=" << strategy->getApiUrl() << std::endl;

    
    if (false == strategy->isMCPModel) {

        addMessage(userId, userName, true, userQuestion, sessionId);
        json payload = strategy->buildRequest(this->messages);

        //执行请求（经过网关）
        json response = executeCurl(payload, userId, modelType);
        std::string answer = strategy->parseResponse(response);
        addMessage(userId, userName, false, answer, sessionId);
        return answer.empty() ? "[Error] 无法解析响应" : answer;
    }
    //说明支持MCP
    AIConfig config;
    config.loadFromFile("../AIApps/ChatServer/resource/config.json");
    std::string tempUserQuestion =config.buildPrompt(userQuestion);
    std::cout << "tempUserQuestion is " << tempUserQuestion << std::endl;
    messages.push_back({ tempUserQuestion, 0 });

    json firstResp;
    std::string aiResult;
    try {
        json firstReq = strategy->buildRequest(this->messages);
        firstResp = executeCurl(firstReq, userId, modelType);
        aiResult = strategy->parseResponse(firstResp);
    } catch (...) {
        messages.pop_back();  // 异常时清理临时消息
        throw;
    }
    messages.pop_back();

    std::cout << "aiResult is " << aiResult << std::endl;
    // 解析AI响应（是否工具调用）
    AIToolCall call = config.parseAIResponse(aiResult);

    // 情况1：AI 不调用工具
    if (!call.isToolCall) {
        addMessage(userId, userName, true, userQuestion, sessionId);
        addMessage(userId, userName, false, aiResult, sessionId);

        std::cout << "No tools required" << std::endl;
        return aiResult;
    }

    // 情况 2：AI 要调用工具
    json toolResult;
    AIToolRegistry registry;

    try {
        toolResult = registry.invoke(call.toolName, call.args);
        std::cout << "Tool call success" << std::endl;
    }
    catch (const std::exception& e) {
        //大多数情况都不会走这里
        std::string err = "[工具调用失败] " + std::string(e.what());
        addMessage(userId, userName, true, userQuestion, sessionId);
        addMessage(userId, userName, false, err, sessionId);

        std::cout << "Tool call failed" << std::endl << std::string(e.what());
        return err;
    }

    // 第二次调用AI
    // 用同样的 prompt_template，但说明工具执行过
    std::string secondPrompt = config.buildToolResultPrompt(userQuestion, call.toolName, call.args, toolResult);
    
    std::cout << "secondPrompt is " << secondPrompt << std::endl;
    messages.push_back({ secondPrompt, 0 });

    json secondResp;
    std::string finalAnswer;
    try {
        json secondReq = strategy->buildRequest(messages);
        secondResp = executeCurl(secondReq, userId, modelType);
        finalAnswer = strategy->parseResponse(secondResp);
    } catch (...) {
        messages.pop_back();  // 异常时清理临时消息
        throw;
    }
    messages.pop_back();

    std::cout << "finalAnswer is " << finalAnswer << std::endl;

    addMessage(userId, userName, true, userQuestion, sessionId);
    addMessage(userId, userName, false, finalAnswer, sessionId);
    return finalAnswer;
}

// 发送聊天消息
std::string AIHelper::chat(int userId,std::string userName, std::string sessionId, std::string userQuestion, std::string modelType) {
    // 持有锁调用 chatInternal，确保 messages 和 strategy 的线程安全
    std::lock_guard<std::mutex> lock(mutex_);
    return chatInternal(userId, userName, sessionId, userQuestion, modelType);
}

// 发送聊天消息，启用流式回复
void AIHelper::chatStreaming(int userId, std::string userName, std::string sessionId, std::string userQuestion, std::string modelType, ChunkCallback onChunk){
    std::lock_guard<std::mutex> lock(mutex_);
    setStrategy(StrategyFactory::instance().create(modelType));
    std::cout << "[AIHelper::chatStreaming] received modelType=" << modelType
                << " -> strategy: model=" << strategy->getModel()
                << " url=" << strategy->getApiUrl() << std::endl;
    // MCP不走流式，直接同步chat
    if (strategy->isMCPModel) {
        std::string answer = chatInternal(userId, userName, sessionId, userQuestion, modelType);
        onChunk(answer);
        return;
    }
    // 非MCP流式
    addMessage(userId, userName, true, userQuestion, sessionId); // 增加一条用户消息
    json payload = strategy->buildRequest(this->messages);
    payload["stream"] = true; // 开启流式响应
    std::string fullresult; // 拼接完整响应
    // 直接调用网关的流式接口，两次回调，一次是每块数据，一次是完整响应
    LLMGateway::instance().callStreaming(
        modelType,
        payload,
        strategy->getApiUrl(),
        strategy->getApiKey(),  
        userId,
        [&](const std::string& chunk
        ) {
            std::string parsedChunk = strategy->parseStreamChunk(chunk);
            if(!parsedChunk.empty()) {
                fullresult += parsedChunk; // 拼接完整响应
                onChunk(parsedChunk); // 每收到一块就回调一次
            }
        }
    );

    addMessage(userId, userName, false, fullresult, sessionId); // 增加一条AI消息
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
