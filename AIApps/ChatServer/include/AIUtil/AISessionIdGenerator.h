#pragma once
#include <chrono>
#include <random>
#include <cstdlib>
#include <ctime>
#include <string>

// ai会话id生成器
class AISessionIdGenerator {
public:
    AISessionIdGenerator() {
        // 生成时间的随机数
        std::srand(static_cast<unsigned>(std::time(nullptr)));
    }
    // 生成器
    std::string generate();
};
