# 方案 B 实施计划：工具路由与最终回答解耦

## 1. 背景与目标

当前 chat 业务存在两条路径：

- `stream=false`：`ChatSendHandler/ChatCreateAndSendHandler -> AIHelper::chat -> AIHelper::chatInternal -> executeCurl -> LLMGateway::call -> executeHttp`
- `stream=true`：`ChatSendHandler/ChatCreateAndSendHandler -> AIHelper::chatStreaming -> LLMGateway::callStreaming -> executeHttpStreaming`

问题在于：

- `stream` 本应只表示客户端希望以 SSE 流式方式接收响应。
- 当前 `stream=true` 会绕过 `chatInternal` 中的 MCP 工具判定逻辑。
- 结果是 `stream` 同时变成了“响应是否流式”和“是否启用工具编排”的共同开关，业务语义不匹配。

方案 B 的目标：

- 第一次 LLM 调用只做“是否需要工具”的路由判断，不直接回答用户。
- 第二次 LLM 调用统一生成最终回答。
- 如果客户端请求 `stream=true`，第二次最终回答调用走真实流式。
- 如果客户端请求 `stream=false`，第二次最终回答调用走普通非流式。
- `stream` 与工具能力解耦，工具能力由单独参数或模型能力控制。

核心原则：

- `stream` 只控制响应传输形态。
- `enableTools` 或 `toolMode` 控制是否允许工具编排。
- 第一次路由调用必须是非流式。
- 最终回答调用根据客户端 `stream` 决定是否流式。
- 不使用分词器模拟流式，优先保证真实上游流式。

## 2. 目标调用流程

### 2.1 不支持工具的模型

当 `strategy->supportTools() == false`：

#### 非流式请求

```text
chat()
  -> 添加用户消息
  -> buildRequestWithContext()
  -> executeCurl()
  -> parseResponse()
  -> 添加 AI 消息
  -> 返回完整答案
```

#### 流式请求

```text
chatStreaming()
  -> 添加用户消息
  -> buildRequestWithContext()
  -> payload["stream"] = true
  -> LLMGateway::callStreaming()
  -> parseStreamChunk()
  -> onChunk()
  -> 累积 fullResult
  -> 添加 AI 消息
```

这部分基本沿用现有逻辑。

### 2.2 支持工具且启用工具的模型

无论客户端是否请求流式，先执行一次非流式路由判断：

```text
用户问题
  -> 构造工具路由 prompt
  -> 第一次 LLM 非流式调用
  -> 解析 ToolDecision
```

然后进入最终回答阶段：

#### 不需要工具

```text
ToolDecision.needTool == false
  -> 使用原始用户问题构造最终回答请求
  -> stream=false: executeCurl()
  -> stream=true : callStreaming()
  -> 落库用户消息和最终 AI 答案
```

#### 需要工具

```text
ToolDecision.needTool == true
  -> AIToolRegistry::invoke()
  -> 构造包含工具结果的最终回答 prompt
  -> stream=false: executeCurl()
  -> stream=true : callStreaming()
  -> 落库用户消息和最终 AI 答案
```

注意：第一次路由调用的输出不能作为最终答案返回。它只决定是否调用工具。

## 3. API 语义调整

### 3.1 请求字段

建议新增一个字段：

```json
{
  "question": "今天北京天气怎么样？",
  "modelType": "aliyun-qwen",
  "stream": true,
  "enableTools": true
}
```

字段语义：

- `stream`：是否使用 SSE 返回最终回答。
- `enableTools`：是否允许工具编排。

### 3.2 默认值

为了兼容现有前端和接口：

- `stream` 默认 `false`。
- `enableTools` 默认 `true`。
- 实际是否进入工具编排还必须同时满足 `strategy->supportTools() == true`。

最终判断：

```cpp
bool useTools = enableTools && strategy->supportTools();
```

如果暂时不想改前端，也可以先不显式传 `enableTools`，后端默认启用即可。这样现有客户端行为会变成：

- `stream=false`：继续支持工具。
- `stream=true`：也支持工具。

## 4. 路由 Prompt 调整

当前 `resource/config.json` 中的 `prompt_template` 允许模型在不需要工具时直接回答用户：

```text
如果只是回答用户的问题或者用户所有参数并没有完全对应的上，请直接输出文本回答。
```

方案 B 下，这个设计需要修改。路由阶段不能让模型回答用户，只能输出结构化决策。

建议改成：

```json
{
  "prompt_template": "你是工具路由器。你的任务是判断用户请求是否需要调用工具，不要回答用户问题。用户输入：{user_input}\n可用工具：\n{tool_list}\n输出要求：只输出 JSON，不要输出 Markdown，不要输出解释文字。\n如果需要工具，输出：{\"need_tool\":true,\"tool\":\"工具名\",\"args\":{\"key\":\"value\"}}\n如果不需要工具，输出：{\"need_tool\":false,\"tool\":\"\",\"args\":{}}\n如果用户缺少工具必需参数或参数无法确定，也输出 need_tool=false。\n"
}
```

建议保留旧格式兼容解析一段时间：

- 新格式：`need_tool == true/false`
- 旧格式：存在 `tool` 字段时视为工具调用
- 普通文本：视为 `need_tool=false`

## 5. 数据结构调整

当前 `AIToolCall` 只有工具调用相关字段。建议扩展成路由决策结构。

位置：

- `AIApps/ChatServer/include/AIUtil/AIConfig.h`

建议结构：

```cpp
struct AIToolCall {
    bool isToolCall = false;
    std::string toolName;
    json args = json::object();
    std::string rawResponse;
};
```

可以维持字段名 `isToolCall`，避免大范围改动；语义上它表示 `need_tool`。

`parseAIResponse()` 调整为：

```cpp
AIToolCall AIConfig::parseAIResponse(const std::string& response) const {
    AIToolCall result;
    result.rawResponse = response;

    try {
        json j = json::parse(response);

        if (j.contains("need_tool") && j["need_tool"].is_boolean()) {
            result.isToolCall = j["need_tool"].get<bool>();
            if (result.isToolCall) {
                result.toolName = j.value("tool", "");
                if (j.contains("args") && j["args"].is_object()) {
                    result.args = j["args"];
                }
            }
            return result;
        }

        // 兼容旧格式：{"tool":"get_weather","args":{...}}
        if (j.contains("tool") && j["tool"].is_string() && !j["tool"].get<std::string>().empty()) {
            result.isToolCall = true;
            result.toolName = j["tool"].get<std::string>();
            if (j.contains("args") && j["args"].is_object()) {
                result.args = j["args"];
            }
        }
    } catch (...) {
        result.isToolCall = false;
    }

    return result;
}
```

## 6. AIHelper 重构计划

当前 `AIHelper::chatInternal()` 同时承担：

- 设置模型策略
- 普通聊天
- 工具路由
- 工具执行
- 最终回答
- 落库

为了避免流式和非流式重复实现，建议拆成几个私有 helper。

位置：

- `AIApps/ChatServer/include/AIUtil/AIHelper.h`
- `AIApps/ChatServer/src/AIUtil/AIHelper.cpp`

### 6.1 新增内部结构

```cpp
struct ToolRouteResult {
    bool needTool = false;
    std::string toolName;
    json args = json::object();
};
```

也可以直接复用 `AIToolCall`，减少新增类型。

### 6.2 新增私有方法

建议新增：

```cpp
void prepareStrategy(const std::string& modelType);

AIToolCall routeToolCall(int userId,
                         const std::string& userQuestion,
                         const std::string& modelType,
                         AIConfig& config);

std::string buildFinalPromptWithOptionalTool(const std::string& userQuestion,
                                             const AIToolCall& call,
                                             const json& toolResult,
                                             const AIConfig& config);

std::string completeOnce(int userId,
                         const std::string& modelType);

void completeStreaming(int userId,
                       const std::string& modelType,
                       ChunkCallback onChunk,
                       std::string& fullResult);
```

职责说明：

- `prepareStrategy()`：统一设置 `strategy` 并打印日志。
- `routeToolCall()`：构造工具路由 prompt，临时压入 `messages`，执行非流式 LLM，解析工具决策，最后弹出临时消息。
- `buildFinalPromptWithOptionalTool()`：根据是否调用工具构造最终回答 prompt。
- `completeOnce()`：基于当前 `messages` 构造请求，执行非流式最终回答。
- `completeStreaming()`：基于当前 `messages` 构造请求，设置 `stream=true`，执行流式最终回答并累积完整答案。

## 7. 新的非流式实现

`chat()` 继续加锁，然后调用新的内部方法。

建议保留：

```cpp
std::string AIHelper::chat(...) {
    std::lock_guard<std::mutex> lock(mutex_);
    return chatInternal(...);
}
```

`chatInternal()` 改成统一编排：

```cpp
std::string AIHelper::chatInternal(int userId,
                                   std::string userName,
                                   std::string sessionId,
                                   std::string userQuestion,
                                   std::string modelType) {
    prepareStrategy(modelType);

    if (!strategy->supportTools()) {
        addMessage(userId, userName, true, userQuestion, sessionId);
        std::string answer = completeOnce(userId, modelType);
        addMessage(userId, userName, false, answer, sessionId);
        return answer.empty() ? "[Error] 无法解析响应" : answer;
    }

    AIConfig config;
    config.loadFromFile("../AIApps/ChatServer/resource/config.json");

    AIToolCall call = routeToolCall(userId, userQuestion, modelType, config);

    json toolResult;
    if (call.isToolCall) {
        AIToolRegistry registry;
        try {
            toolResult = registry.invoke(call.toolName, call.args);
        } catch (const std::exception& e) {
            std::string err = "[工具调用失败] " + std::string(e.what());
            addMessage(userId, userName, true, userQuestion, sessionId);
            addMessage(userId, userName, false, err, sessionId);
            return err;
        }
    }

    std::string finalPrompt = call.isToolCall
        ? config.buildToolResultPrompt(userQuestion, call.toolName, call.args, toolResult)
        : userQuestion;

    messages.push_back({finalPrompt, 0});
    std::string finalAnswer;
    try {
        finalAnswer = completeOnce(userId, modelType);
    } catch (...) {
        messages.pop_back();
        throw;
    }
    messages.pop_back();

    addMessage(userId, userName, true, userQuestion, sessionId);
    addMessage(userId, userName, false, finalAnswer, sessionId);
    return finalAnswer.empty() ? "[Error] 无法解析响应" : finalAnswer;
}
```

注意：

- 最终回答阶段使用 `finalPrompt` 临时放入 `messages`。
- 落库时仍然保存原始 `userQuestion`，不要保存路由 prompt 或工具结果 prompt。
- 非工具场景也会进行第二次 LLM 调用，这是方案 B 的成本代价。

## 8. 新的流式实现

`chatStreaming()` 不再直接调用 `callStreaming()`，而是走与 `chatInternal()` 同样的工具路由。

目标伪代码：

```cpp
void AIHelper::chatStreaming(int userId,
                             std::string userName,
                             std::string sessionId,
                             std::string userQuestion,
                             std::string modelType,
                             ChunkCallback onChunk) {
    std::lock_guard<std::mutex> lock(mutex_);
    prepareStrategy(modelType);

    if (!strategy->supportTools()) {
        addMessage(userId, userName, true, userQuestion, sessionId);
        std::string fullResult;
        completeStreaming(userId, modelType, onChunk, fullResult);
        addMessage(userId, userName, false, fullResult, sessionId);
        return;
    }

    AIConfig config;
    config.loadFromFile("../AIApps/ChatServer/resource/config.json");

    AIToolCall call = routeToolCall(userId, userQuestion, modelType, config);

    json toolResult;
    if (call.isToolCall) {
        AIToolRegistry registry;
        try {
            toolResult = registry.invoke(call.toolName, call.args);
        } catch (const std::exception& e) {
            std::string err = "[工具调用失败] " + std::string(e.what());
            addMessage(userId, userName, true, userQuestion, sessionId);
            addMessage(userId, userName, false, err, sessionId);
            onChunk(err);
            return;
        }
    }

    std::string finalPrompt = call.isToolCall
        ? config.buildToolResultPrompt(userQuestion, call.toolName, call.args, toolResult)
        : userQuestion;

    messages.push_back({finalPrompt, 0});

    std::string fullResult;
    try {
        completeStreaming(userId, modelType, onChunk, fullResult);
    } catch (...) {
        messages.pop_back();
        throw;
    }
    messages.pop_back();

    addMessage(userId, userName, true, userQuestion, sessionId);
    addMessage(userId, userName, false, fullResult, sessionId);
}
```

关键点：

- 第一次工具路由不向客户端发送任何 chunk。
- 第二次最终回答才向客户端发送 chunk。
- 如果不需要工具，也仍然第二次调用并流式输出。
- 如果工具调用失败，可以把错误作为一个 chunk 发给前端，随后正常结束 SSE；也可以抛异常由 handler 发 SSE error，两种方式二选一，建议保持现有非流式行为一致。

## 9. completeStreaming 实现细节

建议从现有 `chatStreaming()` 抽取：

```cpp
void AIHelper::completeStreaming(int userId,
                                 const std::string& modelType,
                                 ChunkCallback onChunk,
                                 std::string& fullResult) {
    json payload = buildRequestWithContext();
    payload["stream"] = true;

    LLMGateway::instance().callStreaming(
        modelType,
        payload,
        strategy->getApiUrl(),
        strategy->getApiKey(),
        userId,
        [&](const std::string& chunk) {
            std::string parsedChunk = strategy->parseStreamChunk(chunk);
            if (!parsedChunk.empty()) {
                fullResult += parsedChunk;
                onChunk(parsedChunk);
            }
        }
    );
}
```

如果后续需要兼容 RAG 或 DashScope 原生流式参数，不要在 `AIHelper` 里写死所有规则。更好的方式是给 `AIStrategy` 增加方法：

```cpp
virtual void enableStreaming(json& payload) const {
    payload["stream"] = true;
}
```

然后改成：

```cpp
strategy->enableStreaming(payload);
```

这样不同策略可以按自己的接口格式设置流式参数。

## 10. routeToolCall 实现细节

```cpp
AIToolCall AIHelper::routeToolCall(int userId,
                                   const std::string& userQuestion,
                                   const std::string& modelType,
                                   AIConfig& config) {
    std::string routePrompt = config.buildPrompt(userQuestion);
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
```

注意：

- 路由调用必须使用 `executeCurl()`，不能使用 `callStreaming()`。
- 路由 prompt 必须临时压入 `messages`，调用结束后弹出。
- 路由 prompt 和路由结果都不能落库。
- 如果 `config.loadFromFile()` 失败，应当选择明确行为：
  - 要么抛异常。
  - 要么降级为不使用工具。

建议先采用“降级为不使用工具”，用户聊天不因工具配置缺失完全失败：

```cpp
if (!config.loadFromFile("../AIApps/ChatServer/resource/config.json")) {
    // fallback: finalPrompt = userQuestion
}
```

## 11. Handler 修改计划

位置：

- `AIApps/ChatServer/src/handlers/ChatSendHandler.cpp`
- `AIApps/ChatServer/src/handlers/ChatCreateAndSendHandler.cpp`

### 11.1 解析 enableTools

新增：

```cpp
bool enableTools = true;
if (j.contains("enableTools")) {
    enableTools = j["enableTools"].get<bool>();
}
```

### 11.2 AIHelper 接口选择

如果要最小改动，可以先不把 `enableTools` 传入 `AIHelper`，只做后端默认启用工具。

更完整的做法是修改接口：

```cpp
std::string chat(..., bool enableTools);
void chatStreaming(..., bool enableTools, ChunkCallback onChunk);
```

然后 handler 调用：

```cpp
if (stream) {
    AIHelperPtr->chatStreaming(..., enableTools, callback);
} else {
    AIHelperPtr->chat(..., enableTools);
}
```

内部判断：

```cpp
bool useTools = enableTools && strategy->supportTools();
```

### 11.3 兼容策略

第一阶段可以不改前端：

- handler 解析 `enableTools`，没有则默认 `true`。
- 前端不传也不影响。

第二阶段再在前端加工具开关。

## 12. 前端修改计划

位置：

- `AIApps/ChatServer/resource/AI.html`

建议新增一个“工具”开关，但不是必须。

如果新增：

- 文案建议：“启用工具”
- 默认开启
- 请求体附加：

```js
{
  question,
  modelType,
  stream,
  enableTools: toolToggle.checked
}
```

如果暂时不新增：

- 后端默认 `enableTools=true`。
- 流式请求自然也会进入工具编排。

## 13. 真实流式诊断计划

你之前遇到“第二次调用启用流式，但仍然整块作为首 token 返回”的问题。方案 B 实施前必须先能定位原因，否则改完仍可能看起来不像流式。

### 13.1 在网关层打印原始 SSE data 行

位置：

- `AIApps/ChatServer/src/LLMGateway/LLMGateway.cpp`
- `streamingWriteCallback()`

临时加入：

```cpp
std::cout << "[stream raw data] size=" << data.size()
          << " preview=" << data.substr(0, 120) << std::endl;
```

判断：

- 如果只有一条 `data:`，且里面是完整回答：上游没有真实增量。
- 如果有很多条 `data:`，说明上游流式正常。

### 13.2 在 AIHelper 层打印解析后的 chunk

位置：

- `completeStreaming()` 或当前 `chatStreaming()` 的 `parseStreamChunk()` 之后

临时加入：

```cpp
std::cout << "[stream parsed chunk] size=" << parsedChunk.size()
          << " content=" << parsedChunk << std::endl;
```

判断：

- 原始 `data:` 多，解析后只有一次：`parseStreamChunk()` 不匹配上游格式。
- 原始 `data:` 多，解析后也多：服务端流式正常，检查 handler 或前端。

### 13.3 检查 parseStreamChunk

当前 `GenericAIStrategy::parseStreamChunk()` 只支持：

```cpp
choices[0].delta.content
```

可以兼容更多格式：

```cpp
if (choice.contains("delta") && choice["delta"].contains("content")) {
    return choice["delta"]["content"];
}
if (choice.contains("message") && choice["message"].contains("content")) {
    return choice["message"]["content"];
}
if (choice.contains("text")) {
    return choice["text"];
}
```

但要注意：

- 如果后端每次返回的是“累计全文”，直接追加会导致重复内容。
- 这种情况下需要在 strategy 中维护已输出长度，或者要求后端开启增量输出参数。
- 不建议在 `AIHelper` 里做通用去重，因为不同模型流格式不同。

### 13.4 检查上游流式参数

OpenAI-compatible 接口通常是：

```json
{
  "stream": true
}
```

但 DashScope 原生接口、RAG 应用或某些兼容接口可能需要不同参数。建议把“开启流式”的逻辑放进 `AIStrategy`：

```cpp
virtual void enableStreaming(json& payload) const;
```

不同策略分别实现：

- `GenericAIStrategy`：`payload["stream"] = true`
- `AliyunRAGStrategy`：按 DashScope RAG 实际要求设置，例如 `payload["parameters"]["incremental_output"] = true`，具体以接口文档和实测为准

## 14. 流式单 chunk 问题的处理策略

如果实施方案 B 后，最终回答仍然只有一个完整 chunk，应按以下顺序排查：

### 14.1 确认第二次调用是否真的进了 callStreaming

日志中应看到：

```text
[AIHelper::completeStreaming]
[LLMGateway::callStreaming]
[stream raw data]
```

如果没有，说明方案 B 接线错误，最终回答仍然走了 `executeCurl()`。

### 14.2 确认请求体是否包含正确 stream 参数

在 `completeStreaming()` 中临时打印：

```cpp
std::cout << "[stream payload] " << payload.dump() << std::endl;
```

确认最终回答请求包含正确流式字段。

### 14.3 确认后端是否返回多条 SSE

根据 `stream raw data` 日志判断。

如果后端只返回一条完整回答：

- 方案 B 无法解决。
- 需要调整模型 API 参数或换支持真实增量流式的接口。

### 14.4 确认 parseStreamChunk 是否吞掉中间 chunk

如果原始 SSE 多条，但 `parsedChunk` 少，优先修 strategy 解析逻辑。

### 14.5 确认前端是否按 SSE 增量渲染

如果服务端 `onChunk()` 多次触发，但前端一次显示：

- 检查 `sendSSEChunk()` 是否 flush。
- 检查前端 fetch reader 是否逐块读取。
- 检查代理或浏览器缓冲。

## 15. 建议提交顺序

### 第一步：只做诊断日志

目的：

- 确认现有纯流式路径是否真的是多 chunk。
- 确认上游返回格式。

改动：

- `LLMGateway.cpp` 打印原始 SSE data。
- `AIHelper.cpp` 打印 parsed chunk。

验证：

- 普通 `stream=true` 聊天是否多条 raw data。

### 第二步：修改工具路由 prompt 和解析

目的：

- 让第一次 LLM 只输出路由 JSON。

改动：

- `resource/config.json`
- `AIConfig::parseAIResponse()`

验证：

- 输入“不需要工具”的问题，第一次输出 `need_tool=false`。
- 输入“北京天气怎么样”，第一次输出 `need_tool=true`。

### 第三步：抽取 completeOnce / completeStreaming

目的：

- 减少 `chatInternal()` 和 `chatStreaming()` 重复逻辑。

改动：

- `AIHelper.h`
- `AIHelper.cpp`

验证：

- 不支持工具模型的非流式和流式行为不变。
- 支持工具模型先临时保持原逻辑，不一次性改完所有路径。

### 第四步：实现 chatInternal 的方案 B

目的：

- 非流式路径也使用“路由调用 + 最终回答调用”。

验证：

- 不需要工具：两次 LLM，第二次完整返回最终回答。
- 需要工具：路由、工具执行、第二次完整返回最终回答。
- 数据库只保存原始用户问题和最终 AI 答案。

### 第五步：实现 chatStreaming 的方案 B

目的：

- 流式路径复用工具路由。
- 最终回答走 `completeStreaming()`。

验证：

- 不需要工具：第一次路由不输出，第二次流式输出最终回答。
- 需要工具：第一次路由不输出，工具执行后第二次流式输出最终回答。

### 第六步：增加 enableTools 参数

目的：

- 完整解耦工具能力和流式传输。

改动：

- 两个 handler 解析 `enableTools`。
- `AIHelper::chat()` 和 `chatStreaming()` 增加参数。
- 内部使用 `enableTools && strategy->supportTools()`。

验证：

- `stream=true, enableTools=false`：直接流式回答，不做工具路由。
- `stream=true, enableTools=true`：先路由，再流式最终回答。
- `stream=false, enableTools=false`：直接普通回答。
- `stream=false, enableTools=true`：先路由，再普通最终回答。

## 16. 测试用例

### 16.1 不需要工具，非流式

请求：

```json
{
  "question": "解释一下 HTTP keep-alive",
  "modelType": "aliyun-qwen",
  "stream": false,
  "enableTools": true
}
```

预期：

- 第一次路由：`need_tool=false`
- 第二次非流式生成最终答案
- HTTP 返回 JSON 完整答案
- 数据库保存原始问题和最终答案

### 16.2 不需要工具，流式

请求：

```json
{
  "question": "解释一下 HTTP keep-alive",
  "modelType": "aliyun-qwen",
  "stream": true,
  "enableTools": true
}
```

预期：

- 第一次路由：`need_tool=false`
- 第二次 `callStreaming()`
- 服务端多次触发 `onChunk()`，前端逐步显示
- 如果只有一个 chunk，进入第 14 节排查

### 16.3 需要工具，非流式

请求：

```json
{
  "question": "北京现在天气怎么样？",
  "modelType": "aliyun-qwen",
  "stream": false,
  "enableTools": true
}
```

预期：

- 第一次路由：`need_tool=true, tool=get_weather`
- 执行工具
- 第二次非流式生成最终答案
- 数据库保存原始问题和最终答案

### 16.4 需要工具，流式

请求：

```json
{
  "question": "北京现在天气怎么样？",
  "modelType": "aliyun-qwen",
  "stream": true,
  "enableTools": true
}
```

预期：

- 第一次路由不向客户端输出
- 工具执行成功
- 第二次 `callStreaming()`
- 前端流式显示基于工具结果的最终答案

### 16.5 关闭工具，流式

请求：

```json
{
  "question": "北京现在天气怎么样？",
  "modelType": "aliyun-qwen",
  "stream": true,
  "enableTools": false
}
```

预期：

- 不执行路由
- 不执行工具
- 直接流式回答

### 16.6 工具调用失败

请求：

```json
{
  "question": "查询一个不存在工具的数据",
  "modelType": "aliyun-qwen",
  "stream": true,
  "enableTools": true
}
```

预期：

- 如果路由到了不存在工具，返回工具失败错误
- SSE 正常结束或返回 SSE error
- 数据库保存原始问题和错误答案

## 17. 风险与取舍

### 17.1 成本与延迟增加

方案 B 下，只要启用工具路由，所有支持工具的请求都至少两次 LLM 调用：

- 第一次：路由判断
- 第二次：最终回答

优点：

- 语义清晰
- 最终回答可以统一流式
- 不需要模拟分词流式

缺点：

- 延迟增加
- Token 成本增加

### 17.2 路由误判

路由模型可能误判工具需求。缓解方式：

- 路由 prompt 明确“只判断，不回答”。
- 输出强制 JSON。
- 对 JSON 解析失败降级为不使用工具。
- 后续可以引入更小更快的模型做路由。

### 17.3 上游不支持真实流式

如果最终回答调用的上游只返回一个完整 chunk，方案 B 无法解决真实流式体验。必须：

- 调整请求参数。
- 修正 strategy 的流式解析。
- 更换支持真实 SSE 增量的接口或模型。

### 17.4 上下文污染

路由 prompt 和工具结果 prompt 都是临时消息，不能落库，也不能长期留在 `messages`。

所有临时 `messages.push_back()` 必须配对 `pop_back()`，并使用 `try/catch` 保证异常时清理。

## 18. 最终验收标准

完成后应满足：

- `stream` 不再决定是否支持工具。
- `enableTools=false` 时不会进入工具路由。
- `enableTools=true && supportTools=true` 时，流式和非流式都支持工具。
- 工具路由第一次调用永远非流式，且不向客户端输出。
- 最终回答调用根据 `stream` 决定普通返回或 SSE 返回。
- 数据库只保存原始用户问题和最终 AI 答案。
- 路由 prompt、工具结果 prompt、路由 JSON 不进入历史消息。
- 对不需要工具的流式请求，最终回答来自第二次真实流式 LLM 调用。
- 如果仍然只有单个 chunk，可以通过日志明确判断是上游、解析、服务端发送还是前端渲染问题。

