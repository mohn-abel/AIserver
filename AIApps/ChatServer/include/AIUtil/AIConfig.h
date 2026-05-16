#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <regex>
#include <fstream>
#include <sstream>
#include <iostream>
#include "../../../../HttpServer/include/utils/JsonUtil.h"  // 假设封装了 nlohmann::json

// 该文件定义了 AI 配置管理相关的结构体和类，
// 用于处理工具调用、提示构建和响应解析。

// 结构体：单个工具信息
struct AITool {
    std::string name;  // 工具名称
    std::unordered_map<std::string, std::string> params;  // 工具参数映射（参数名 -> 参数描述）
    std::string desc;  // 工具描述
};
// 结构体：AI 响应中工具调用结果
struct AIToolCall {
    std::string toolName;  // 被调用的工具名称
    json args;  // 工具调用参数（JSON 格式）
    bool isToolCall = false;  // 标志是否为工具调用
};

// 配置管理类：负责加载配置、构建提示和解析响应
class AIConfig {
public:
    // 从配置文件加载 AI 配置（包括提示模板和工具列表）
    // 参数 path: 配置文件路径
    // 返回: 加载成功返回 true，否则 false
    bool loadFromFile(const std::string& path);

    // 根据用户输入和配置构建发送给 AI 的提示文本
    // 参数 userInput: 用户输入内容
    // 返回: 完整的提示字符串
    std::string buildPrompt(const std::string& userInput) const;

    // 解析 AI 返回的响应，提取工具调用信息
    // 参数 response: AI 的原始响应文本
    // 返回: 解析出的工具调用结构体
    AIToolCall parseAIResponse(const std::string& response) const;

    // 构建包含工具执行结果的提示，用于继续对话
    // 参数 userInput: 原始用户输入
    // 参数 toolName: 执行的工具名称
    // 参数 toolArgs: 工具调用参数
    // 参数 toolResult: 工具执行结果
    // 返回: 包含结果的提示字符串
    std::string buildToolResultPrompt(const std::string& userInput,
                                      const std::string& toolName,
                                      const json& toolArgs,
                                      const json& toolResult) const;

private:
    std::string promptTemplate_;  // 提示模板字符串
    std::vector<AITool> tools_;   // 可用的工具列表

    // 私有方法：根据工具列表构建工具描述字符串
    // 返回: 格式化的工具列表文本
    std::string buildToolList() const;
};
