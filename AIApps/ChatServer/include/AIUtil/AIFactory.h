#pragma once
#include <string>
#include <vector>
#include <utility>
#include <iostream>
#include <sstream>
#include <memory>
#include <functional>
#include <unordered_map>
#include <fstream>


#include"AIStrategy.h"

// 该文件实现了策略工厂模式，用于动态注册和创建不同类型的 AI 策略对象。
// 通过 StrategyFactory 单例管理策略创建器，StrategyRegister 模板类简化注册过程。

class StrategyFactory {

public:
    // 定义创建器类型：一个返回 std::shared_ptr<AIStrategy> 的函数对象
    using Creator = std::function<std::shared_ptr<AIStrategy>()>;

    // 获取单例实例，确保全局只有一个 StrategyFactory 对象
    static StrategyFactory& instance();

    // 注册策略：将策略名称与对应的创建器函数关联起来
    // 参数 name: 策略的唯一标识符（如 "1", "2" 等）
    // 参数 creator: 创建该策略对象的函数，通常是 lambda 表达式
    void registerStrategy(const std::string& name, Creator creator);

    // 根据名称创建策略对象，如果未注册则抛出异常
    std::shared_ptr<AIStrategy> create(const std::string& name);

    // 从 JSON 配置文件加载模型定义，自动注册为 GenericAIStrategy
    // 配置文件格式参见 model_config.json
    void loadFromConfig(const std::string& configPath);

    // 获取默认模型 ID
    const std::string& getDefaultModel() const { return defaultModel_; }

    // 解析别名：如果 name 是别名则返回实际 ID，否则原样返回
    std::string resolveAlias(const std::string& name) const;

private:
    // 私有构造函数，防止外部实例化（单例模式）
    StrategyFactory() = default;
    // 存储策略名称到创建器函数的映射表
    std::unordered_map<std::string, Creator> creators;
    // 别名映射：旧 ID → 新 ID
    std::unordered_map<std::string, std::string> aliases_;
    // 默认模型 ID
    std::string defaultModel_ = "aliyun-qwen";
};



// 注意：这里不是 static std::shared_ptr<AIStrategy> instance = std::make_shared<T>();
// 而是为每个 map 条目创建独立的实例，确保每个策略对象独立。
// 没有使用 static 保证每个注册的策略都能正确实例化，但不保证线程安全。

// StrategyRegister 模板类：用于简化策略注册过程
// 通过在全局作用域定义静态对象，自动注册策略到工厂中
template<typename T>
struct StrategyRegister {
    // 构造函数：在对象构造时自动注册策略
    // 参数 name: 策略的标识符，用于后续创建时查找
    StrategyRegister(const std::string& name) {
        // 调用工厂的 registerStrategy 方法，传入名称和创建器 lambda
        StrategyFactory::instance().registerStrategy(name, [] {
            // 创建策略对象的智能指针实例
            std::shared_ptr<AIStrategy> instance = std::make_shared<T>();
            return instance;
            });
    }
};

