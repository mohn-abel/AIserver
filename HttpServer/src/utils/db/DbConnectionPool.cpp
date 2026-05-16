#include "../../../include/utils/db/DbConnectionPool.h"
#include "../../../include/utils/db/DbException.h"
#include <muduo/base/Logging.h>

namespace http 
{
namespace db 
{

void DbConnectionPool::init(const std::string& host,
                          const std::string& user,
                          const std::string& password,
                          const std::string& database,
                          size_t poolSize) 
{
    // 连接池会被多个线程访问，所以操作其成员变量时需要加锁
    std::lock_guard<std::mutex> lock(mutex_);
    // 确保只初始化一次
    if (initialized_) 
    {
        return;
    }

    host_ = host;
    user_ = user;
    password_ = password;
    database_ = database;

    // 创建连接
    for (size_t i = 0; i < poolSize; ++i) 
    {
        connections_.push(createConnection());
    }

    initialized_ = true;
    LOG_INFO << "Database connection pool initialized with " << poolSize << " connections";
}

DbConnectionPool::DbConnectionPool() 
{
    checkThread_ = std::thread(&DbConnectionPool::checkConnections, this);
}

DbConnectionPool::~DbConnectionPool() 
{
    // 通知检查线程退出
    stop_ = true;
    if (checkThread_.joinable())
    {
        checkThread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    while (!connections_.empty()) 
    {
        connections_.pop();
    }
    LOG_INFO << "Database connection pool destroyed";
}

// 修改获取连接的函数
std::shared_ptr<DbConnection> DbConnectionPool::getConnection() 
{
    std::shared_ptr<DbConnection> conn;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        
        while (connections_.empty()) 
        {
            if (!initialized_) 
            {
                throw DbException("Connection pool not initialized");
            }
            LOG_INFO << "Waiting for available connection...";
            cv_.wait(lock);
        }
        
        conn = connections_.front();
        connections_.pop();
    } // 释放锁
    
    try 
    {
        // 在锁外检查连接（DbConnection::ping 内部有自己的 mutex 保护）
        if (!conn->ping()) 
        {
            LOG_WARN << "Connection lost, attempting to reconnect...";
            conn->reconnect();
        }
        
        return std::shared_ptr<DbConnection>(conn.get(), 
            [this, conn](DbConnection*) {
                std::lock_guard<std::mutex> lock(mutex_);
                connections_.push(conn);
                cv_.notify_one();
            });
    } 
    catch (const std::exception& e) 
    {
        LOG_ERROR << "Failed to get connection: " << e.what();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connections_.push(conn);
            cv_.notify_one();
        }
        throw;
    }
}

std::shared_ptr<DbConnection> DbConnectionPool::createConnection() 
{
    return std::make_shared<DbConnection>(host_, user_, password_, database_);
}

// 连接健康检查线程：从池中取出空闲连接检查，检查完放回
void DbConnectionPool::checkConnections() 
{
    while (!stop_) 
    {
        // 每60秒检查一次
        for (int i = 0; i < 60 && !stop_; ++i)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (stop_) break;

        try 
        {
            size_t poolSize = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                poolSize = connections_.size();
            }

            // 逐个取出连接检查，再放回
            for (size_t i = 0; i < poolSize && !stop_; ++i)
            {
                std::shared_ptr<DbConnection> conn;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (connections_.empty())
                    {
                        break;
                    }
                    conn = connections_.front();
                    connections_.pop();
                }

                // 在锁外检查连接（DbConnection 内部有 mutex 保护）
                if (!conn->ping()) 
                {
                    try 
                    {
                        conn->reconnect();
                        LOG_INFO << "Connection reconnected successfully";
                    } 
                    catch (const std::exception& e) 
                    {
                        LOG_ERROR << "Failed to reconnect: " << e.what();
                    }
                }

                // 放回池中
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    connections_.push(conn);
                    cv_.notify_one();
                }
            }
        } 
        catch (const std::exception& e) 
        {
            LOG_ERROR << "Error in check thread: " << e.what();
        }
    }
}

} // namespace db
} // namespace http
