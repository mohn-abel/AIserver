#include"../include/AIUtil/AIFactory.h"


StrategyFactory& StrategyFactory::instance() {
    static StrategyFactory factory;
    return factory;
}

void StrategyFactory::registerStrategy(const std::string& name, Creator creator) {
    creators[name] = std::move(creator);
}

std::shared_ptr<AIStrategy> StrategyFactory::create(const std::string& name) {
    std::string resolved = resolveAlias(name);
    auto it = creators.find(resolved);
    if (it == creators.end()) {
        throw std::runtime_error("Unknown strategy: " + name);
    }
    return it->second();
}

std::string StrategyFactory::resolveAlias(const std::string& name) const {
    auto it = aliases_.find(name);
    return (it != aliases_.end()) ? it->second : name;
}

void StrategyFactory::loadFromConfig(const std::string& configPath) {
    std::ifstream f(configPath);
    if (!f.is_open()) {
        std::cerr << "[Warning] Cannot open model config: " << configPath
                  << ", using built-in defaults" << std::endl;
        return;
    }

    json config = json::parse(f);

    // 加载默认模型
    if (config.contains("default_model")) {
        defaultModel_ = config["default_model"].get<std::string>();
    }

    // 加载别名映射（兼容旧版数字 ID）
    if (config.contains("aliases")) {
        for (auto& [alias, target] : config["aliases"].items()) {
            aliases_[alias] = target.get<std::string>();
        }
    }

    // 加载模型定义并注册
    for (const auto& model : config["models"]) {
        std::string id   = model["id"];
        std::string name = model["name"];
        std::string url  = model["api_url"];
        std::string key  = model.value("api_key", "");
        std::string type = model.value("type", "mcp");
        int maxContext = model.value("max_context", 0);

        if (type == "rag") {
            registerStrategy(id, [=] {
                return std::make_shared<AliyunRAGStrategy>(name, url, key, maxContext);
            });
        } else {
            registerStrategy(id, [=] {
                return std::make_shared<GenericAIStrategy>(name, url, key, maxContext);
            });
        }

    }

    std::cout << "[Model] Default model: " << defaultModel_ << std::endl;
}
