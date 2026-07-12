#pragma once
#include <deque>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>
#include "DbConnection.h"

namespace http
{
namespace db
{

class DbConnectionPool;

struct PoolDeleter {
    DbConnectionPool* pool;
    void operator()(DbConnection* conn) const;
};

// 连接条目：持有连接 + 业务归还时间 + 后台探活时间
struct ConnEntry {
    std::unique_ptr<DbConnection, PoolDeleter> conn;
    std::chrono::steady_clock::time_point lastReturned;
    std::chrono::steady_clock::time_point lastChecked;
};

class DbConnectionPool
{
    friend struct PoolDeleter;

public:
    // 单例模式
    static DbConnectionPool& getInstance()
    {
        static DbConnectionPool instance;
        return instance;
    }

    // 初始化连接池
    void init(const std::string& host,
             const std::string& user,
             const std::string& password,
             const std::string& database,
             size_t poolSize = 10);

    // 获取连接
    std::unique_ptr<DbConnection, PoolDeleter> getConnection();

    // 连接信息（供 PoolDeleter/checkConnections 重建连接时使用）
    const std::string& getHost()     const { return host_; }
    const std::string& getUser()     const { return user_; }
    const std::string& getPassword() const { return password_; }
    const std::string& getDatabase() const { return database_; }

private:
    // 构造函数
    DbConnectionPool();
    // 析构函数
    ~DbConnectionPool();

    // 禁止拷贝
    DbConnectionPool(const DbConnectionPool&) = delete;
    DbConnectionPool& operator=(const DbConnectionPool&) = delete;

    std::unique_ptr<DbConnection, PoolDeleter> createConnection();

    void checkConnections();
    void returnConnection(DbConnection* conn);

    // 创建一个全新连接，用于替换坏连接
    std::unique_ptr<DbConnection, PoolDeleter> spawnReplacement();

private:
    std::string                               host_;
    std::string                               user_;
    std::string                               password_;
    std::string                               database_;
    // deque 头部是冷连接，尾部是热连接；业务按 LIFO 从尾部复用
    std::deque<ConnEntry>                     connections_;
    std::mutex                                mutex_;
    std::condition_variable                   cv_;
    bool                                      initialized_ = false;
    std::atomic<bool>                         stop_{false};
    std::thread                               checkThread_;
    std::chrono::seconds                      idleThreshold_{60};   // 空闲超过此阈值才探活
    std::chrono::seconds                      healthCheckInterval_{60}; // 同一空闲连接的最小探活间隔
    int                                       maxCheckPerRound_{5}; // 每轮最多检查数
    std::chrono::milliseconds                 acquireTimeout_{5000};// 获取连接的最大等待时间，超时抛异常
};

} // namespace db
} // namespace http
