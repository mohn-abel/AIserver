#pragma once
#include <string>
#include <vector>
#include <utility>
#include <iostream>
#include <sstream>
#include <memory>

#include "../../../../HttpServer/include/utils/JsonUtil.h"

// 抽象基类：定义 AI 策略的接口
class AIStrategy {
public:
    virtual ~AIStrategy() = default;

    virtual std::string getApiUrl() const = 0;
    virtual std::string getApiKey() const = 0;
    virtual std::string getModel() const = 0;

    virtual json buildRequest(const std::vector<std::pair<std::string, long long>>& messages) const = 0;
    virtual std::string parseResponse(const json& response) const = 0;
    virtual std::string parseStreamChunk(const std::string& jsonStr) const = 0;
    bool isMCPModel = false;
};

// 通用策略类：支持所有 OpenAI 兼容 API（DeepSeek、OpenAI、Ollama、vLLM 等）
// 所有参数从构造函数传入，不再硬编码
class GenericAIStrategy : public AIStrategy {
public:
    GenericAIStrategy(const std::string& modelName,
                      const std::string& apiUrl,
                      const std::string& apiKey,
                      bool isMcp = false)
        : modelName_(modelName), apiUrl_(apiUrl), apiKey_(apiKey)
    {
        isMCPModel = isMcp;
    }

    std::string getApiUrl() const override { return apiUrl_; }
    std::string getApiKey() const override { return apiKey_; }
    std::string getModel() const override { return modelName_; }

    json buildRequest(const std::vector<std::pair<std::string, long long>>& messages) const override {
        json payload;
        payload["model"] = modelName_;
        json msgArray = json::array();
        for (size_t i = 0; i < messages.size(); ++i) {
            json msg;
            msg["role"] = (i % 2 == 0) ? "user" : "assistant";
            msg["content"] = messages[i].first;
            msgArray.push_back(msg);
        }
        payload["messages"] = msgArray;
        return payload;
    }

    std::string parseResponse(const json& response) const override {
        if (response.contains("choices") && !response["choices"].empty()) {
            return response["choices"][0]["message"]["content"];
        }
        return {};
    }

    std::string parseStreamChunk(const std::string& jsonStr) const override {
        try{
            auto j = json::parse(jsonStr);
            if(j.contains("choices") && !j["choices"].empty()) {
                auto choice = j["choices"][0];
                if (choice.contains("delta") && choice["delta"].contains("content")) {
                    return choice["delta"]["content"];
                }
            }
            return {};
        }catch(...){
            return {};
        }
    }

private:
    std::string modelName_;
    std::string apiUrl_;
    std::string apiKey_;
};

// RAG 策略类：阿里云 DashScope RAG 专用格式
// 请求格式为 input.prompt，响应解析路径为 output.text
class AliyunRAGStrategy : public AIStrategy {
public:
    AliyunRAGStrategy(const std::string& modelName,
                      const std::string& apiUrl,
                      const std::string& apiKey)
        : modelName_(modelName), apiUrl_(apiUrl), apiKey_(apiKey)
    {
        isMCPModel = false;
    }

    std::string getApiUrl() const override { return apiUrl_; }
    std::string getApiKey() const override { return apiKey_; }
    std::string getModel() const override { return modelName_; }

    json buildRequest(const std::vector<std::pair<std::string, long long>>& messages) const override {
        std::ostringstream prompt;
        for (size_t i = 0; i < messages.size(); ++i) {
            prompt << ((i % 2 == 0) ? "用户: " : "助手: ") << messages[i].first << "\n";
        }
        json payload;
        payload["input"]["prompt"] = prompt.str();
        payload["parameters"] = json::object();
        return payload;
    }

    std::string parseResponse(const json& response) const override {
        if (response.contains("output") && response["output"].contains("text")) {
            return response["output"]["text"];
        }
        if (response.contains("message")) {
            return "[RAG Error] " + response["message"].get<std::string>();
        }
        return "[RAG Error] Unexpected response: " + response.dump();
    }

    std::string parseStreamChunk(const std::string& jsonStr) const override {
        try{
            auto j = json::parse(jsonStr);
            if(j.contains("output") && j["output"].contains("text")) {
                return j["output"]["text"];
            }
            return {};
        }catch(...){
            return {};
        }
    }

private:
    std::string modelName_;
    std::string apiUrl_;
    std::string apiKey_;
};
