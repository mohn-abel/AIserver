#include "../../../include/utils/db/DbConnectionPool.h"
#include "../../../include/utils/db/DbException.h"
#include <muduo/base/Logging.h>

namespace http
{
namespace db
{

// ── PoolDeleter + returnConnection ────────────────────────────

void PoolDeleter::operator()(DbConnection* conn) const
{
    pool->returnConnection(conn);
}

void DbConnectionPool::returnConnection(DbConnection* conn)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // LIFO: 归还到 vector 尾部，打上时间戳
    connections_.push_back({std::unique_ptr<DbConnection, PoolDeleter>(conn, PoolDeleter{this}),
                            std::chrono::steady_clock::now()});
    cv_.notify_one();
}

// ── 初始化 ────────────────────────────────────────────────────

void DbConnectionPool::init(const std::string& host,
                          const std::string& user,
                          const std::string& password,
                          const std::string& database,
                          size_t poolSize)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_)
    {
        return;
    }

    host_     = host;
    user_     = user;
    password_ = password;
    database_ = database;

    for (size_t i = 0; i < poolSize; ++i)
    {
        connections_.push_back({createConnection(), std::chrono::steady_clock::now()});
    }

    initialized_ = true;
    LOG_INFO << "Database connection pool initialized with " << poolSize << " connections (LIFO)";
}

DbConnectionPool::DbConnectionPool()
{
    checkThread_ = std::thread(&DbConnectionPool::checkConnections, this);
}

DbConnectionPool::~DbConnectionPool()
{
    stop_ = true;
    if (checkThread_.joinable())
    {
        checkThread_.join();
    }

    // 唯一真正释放连接的地方：release 绕过 PoolDeleter，再 delete 裸指针
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : connections_)
    {
        DbConnection* raw = entry.conn.release();
        delete raw;
    }
    connections_.clear();
    LOG_INFO << "Database connection pool destroyed";
}

// ── createConnection / spawnReplacement ──────────────────────

std::unique_ptr<DbConnection, PoolDeleter> DbConnectionPool::createConnection()
{
    return std::unique_ptr<DbConnection, PoolDeleter>(
        new DbConnection(host_, user_, password_, database_),
        PoolDeleter{this});
}

std::unique_ptr<DbConnection, PoolDeleter> DbConnectionPool::spawnReplacement()
{
    return createConnection();
}

// ── 获取连接（LIFO：总是取栈顶热连接）─────────────────────────

std::unique_ptr<DbConnection, PoolDeleter> DbConnectionPool::getConnection()
{
    std::unique_ptr<DbConnection, PoolDeleter> conn;
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

        // LIFO: 从尾部取（最后归还的 = 最热的）
        conn = std::move(connections_.back().conn);
        connections_.pop_back();
    }

    try
    {
        if (!conn->ping())
        {
            LOG_WARN << "Connection lost, attempting to reconnect...";
            try
            {
                conn->reconnect();
            }
            catch (const std::exception& e)
            {
                LOG_ERROR << "Reconnect failed: " << e.what() << ", replacing with new connection";
                conn = spawnReplacement();   // 坏连接被 unique_ptr 析构，替换为新连接
            }
        }

        return conn;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR << "Failed to get connection: " << e.what();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connections_.push_back({std::move(conn), std::chrono::steady_clock::now()});
            cv_.notify_one();
        }
        throw;
    }
}

// ── 健康检查：时间戳过滤 + 采样上限 + 坏连接替换 ───────────────

void DbConnectionPool::checkConnections()
{
    while (!stop_)
    {
        for (int i = 0; i < 60 && !stop_; ++i)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (stop_) break;

        try
        {
            auto now = std::chrono::steady_clock::now();
            int checked = 0;

            // 从栈底（index 0，idle 最长）向栈顶扫描，swap+pop 取出超时连接
            size_t i = 0;
            while (i < connections_.size() && checked < maxCheckPerRound_ && !stop_)
            {
                ConnEntry entry;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (i >= connections_.size()) break;

                    auto idle = now - connections_[i].lastReturned;
                    if (idle < idleThreshold_)
                    {
                        ++i;       // 这个连接还热，跳过
                        continue;
                    }

                    // 取出 connections_[i]：与尾部交换后 pop_back
                    entry = std::move(connections_[i]);
                    if (i != connections_.size() - 1)
                    {
                        connections_[i] = std::move(connections_.back());
                    }
                    connections_.pop_back();
                    // 不递增 i，因为现在 connections_[i] 是之前尾部的元素
                }

                // 锁外 ping，不阻塞业务线程
                if (!entry.conn->ping())
                {
                    try
                    {
                        entry.conn->reconnect();
                        LOG_INFO << "Connection reconnected successfully";
                    }
                    catch (const std::exception& e)
                    {
                        LOG_ERROR << "Reconnect failed, replacing: " << e.what();
                        entry.conn = spawnReplacement();
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    entry.lastReturned = std::chrono::steady_clock::now();
                    connections_.push_back(std::move(entry));
                    cv_.notify_one();
                }
                ++checked;
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
