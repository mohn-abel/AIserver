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
    auto now = std::chrono::steady_clock::now();
    // LIFO: 业务归还到 deque 尾部，尾部保持为热连接区域
    connections_.push_back({std::unique_ptr<DbConnection, PoolDeleter>(conn, PoolDeleter{this}),
                            now,
                            now});
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
        auto now = std::chrono::steady_clock::now();
        connections_.push_back({createConnection(), now, now});
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

// ── 获取连接（LIFO：总是取尾部热连接）─────────────────────────

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
            // 带超时等待：避免连接耗尽时业务线程永久阻塞，超时则快速失败
            if (cv_.wait_for(lock, acquireTimeout_,
                             [this] { return !connections_.empty(); }))
            {
                break;  // 谓词为真：有可用连接
            }
            // 超时仍无可用连接
            throw DbException("Timeout acquiring database connection from pool");
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
            auto now = std::chrono::steady_clock::now();
            connections_.push_back({std::move(conn), now, now});
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
            int checked = 0;
            size_t i = 0;

            // deque 头部是冷连接，尾部是热连接；健康检查只扫描冷端。
            while (checked < maxCheckPerRound_ && !stop_)
            {
                ConnEntry entry;
                size_t insertIndex = 0;

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (i >= connections_.size()) break;

                    auto now = std::chrono::steady_clock::now();
                    auto idle = now - connections_[i].lastReturned;
                    if (idle < idleThreshold_)
                    {
                        break;  // 冷端都不够冷，后面的连接更不需要探活
                    }

                    auto sinceCheck = now - connections_[i].lastChecked;
                    if (sinceCheck < healthCheckInterval_)
                    {
                        ++i;    // 仍是冷连接，但刚探活过，继续看下一个冷连接
                        continue;
                    }

                    // 保序摘出待检查连接，不触碰尾部热连接。
                    insertIndex = i;
                    entry = std::move(connections_[i]);
                    connections_.erase(connections_.begin() + i);
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
                    entry.lastChecked = std::chrono::steady_clock::now();
                    if (insertIndex > connections_.size())
                    {
                        insertIndex = connections_.size();
                    }
                    connections_.insert(connections_.begin() + insertIndex, std::move(entry));
                    cv_.notify_one();
                }

                ++checked;
                ++i;
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
