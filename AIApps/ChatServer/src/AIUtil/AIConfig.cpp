#include"../include/AIUtil/AIConfig.h"

// 从配置文件加载 AI 配置（提示模板和工具列表）
bool AIConfig::loadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[AIConfig] 无法打开配置文件: " << path << std::endl;
        return false;
    }

    json j;
    file >> j;

    // 解析提示模板
    if (!j.contains("prompt_template") || !j["prompt_template"].is_string()) {
        std::cerr << "[AIConfig] 缺少 prompt_template 字段" << std::endl;
        return false;
    }
    promptTemplate_ = j["prompt_template"].get<std::string>();

    // 解析工具列表
    if (j.contains("tools") && j["tools"].is_array()) {
        for (auto& tool : j["tools"]) {
            AITool t;
            t.name = tool.value("name", "");
            t.desc = tool.value("desc", "");
            // 解析工具参数
            if (tool.contains("params") && tool["params"].is_object()) {
                for (auto& [key, val] : tool["params"].items()) {
                    t.params[key] = val.get<std::string>();
                }
            }
            tools_.push_back(std::move(t));
        }
    }
    return true;
}

// 构建工具列表描述字符串，格式为 "工具名(参数1, 参数2) - 工具描述"
std::string AIConfig::buildToolList() const {
    std::ostringstream oss;
    for (const auto& t : tools_) {
        oss << t.name << "(";
        bool first = true;
        // 列出所有参数名
        for (const auto& [key, val] : t.params) {
            if (!first) oss << ", ";
            oss << key;
            first = false;
        }
        // 添加工具描述
        oss << ") - " << t.desc << "\n";
    }
    return oss.str();
}

// 根据用户输入和配置构建完整的提示文本
std::string AIConfig::buildPrompt(const std::string& userInput) const {
    std::string result = promptTemplate_;
    // 替换模板中的占位符 {user_input}
    result = std::regex_replace(result, std::regex("\\{user_input\\}"), userInput);
    // 替换模板中的占位符 {tool_list}
    result = std::regex_replace(result, std::regex("\\{tool_list\\}"), buildToolList());
    return result;
}

// 解析 AI 响应，提取工具调用信息
AIToolCall AIConfig::parseAIResponse(const std::string& response) const {
    AIToolCall result;
    try {
        // 尝试将响应解析为 JSON
        json j = json::parse(response);

        // 检查是否包含工具调用指令
        if (j.contains("tool") && j["tool"].is_string()) {
            result.toolName = j["tool"].get<std::string>();
            // 提取工具参数
            if (j.contains("args") && j["args"].is_object()) {
                result.args = j["args"];
            }
            result.isToolCall = true;
        }
    }
    catch (...) {
        // 不是 JSON 格式，直接返回普通文本响应
        result.isToolCall = false;
    }
    return result;
}

// 构建包含工具执行结果的提示文本，用于继续对话
std::string AIConfig::buildToolResultPrompt(
    const std::string& userInput,
    const std::string& toolName,
    const json& toolArgs,
    const json& toolResult) const
{
    std::ostringstream oss;
    oss << "用户原始输入: " << userInput << "\n"
        << "已调用工具 [" << toolName << "] 参数: "
        << toolArgs.dump() << "\n"
        << "工具执行结果: \n" << toolResult.dump(4) << "\n"
        << "请基于上述工具执行结果继续响应用户的请求。";
    return oss.str();
}

